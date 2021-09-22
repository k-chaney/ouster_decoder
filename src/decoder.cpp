#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <tf2_ros/static_transform_broadcaster.h>

#include "ouster_ros/OSConfigSrv.h"
#include "ouster_ros/PacketMsg.h"
#include "ouster_ros/ros.h"

namespace ouster_decoder {

namespace os = ouster_ros::sensor;

// We use PointXYZRGB to store extra information
// where (R - Reflectivity, G - signal, B - ambient)
using PointT = pcl::PointXYZI;
using CloudT = pcl::PointCloud<PointT>;

constexpr float kMmToM = 0.001;
constexpr double kTau = 2 * M_PI;
constexpr double kDefaultGravity = 9.807;  // [m/s^2] earth gravity
constexpr float kFloatNaN = std::numeric_limits<float>::quiet_NaN();

constexpr double Deg2Rad(double deg) { return deg * kTau / 360.0; }

/// @brief Convert a vector of double from deg to rad
std::vector<double> TransformDeg2Rad(const std::vector<double>& degs) {
  std::vector<double> rads;
  rads.reserve(degs.size());
  for (const auto& deg : degs) {
    rads.push_back(Deg2Rad(deg));
  }
  return rads;
}

/// @brief image data in scan
struct ImageData {
  float x{};
  float y{};
  float z{};
  float r{};
} __attribute__((packed));

static_assert(sizeof(ImageData) == sizeof(float) * 4,
              "Size of ImageData must be 4 floats (16 bytes)");

/// @brief Stores SensorInfo from ouster with some other useful data
struct LidarModel {
  LidarModel() = default;
  explicit LidarModel(const std::string& metadata) {
    info = os::parse_metadata(metadata);
    pf = &os::get_format(info);

    rows = info.beam_altitude_angles.size();
    cols = os::n_cols_of_lidar_mode(info.mode);
    freq = os::frequency_of_lidar_mode(info.mode);

    dt_col = 1.0 / freq / cols;
    d_azimuth = kTau / cols;
    dt_packet = dt_col * pf->columns_per_packet;
    beam_offset = info.lidar_origin_to_beam_origin_mm * kMmToM;
    altitudes = TransformDeg2Rad(info.beam_altitude_angles);
    azimuths = TransformDeg2Rad(info.beam_azimuth_angles);
  }

  int rows{};                     // number of beams
  int cols{};                     // cols of a full scan
  int freq{};                     // frequency
  double dt_col{};                // delta time between two columns [s]
  double dt_packet{};             // delta time between two packets [s]
  double d_azimuth{};             // delta angle between two columns [rad]
  double beam_offset{};           // distance between beam to origin
  std::vector<double> altitudes;  // altitude angles, high to low [rad]
  std::vector<double> azimuths;   // azimuths offset angles [rad]
  os::sensor_info info;           // sensor info
  os::packet_format const* pf{nullptr};  // packet format

  [[nodiscard]] const auto& pixel_shifts() const noexcept {
    return info.format.pixel_shift_by_row;
  }

  /// @brief Convert lidar range data to xyz
  /// @details see software manual 3.1.2 Lidar Range to XYZ
  ///
  ///    y    r
  ///    ^   / -> rotate clockwise
  ///    |  /
  ///    | /
  ///    |/  theta
  ///    o ---------> x  (connector)
  ///
  [[nodiscard]] Eigen::Vector3f ToPoint(float range,
                                        float theta_enc,
                                        int row) const {
    const float n = beam_offset;
    const float d = range - n;
    const float phi = altitudes[row];
    const float cos_phi = std::cos(phi);
    const float theta = theta_enc - azimuths[row];

    return {d * std::cos(theta) * cos_phi + n * std::cos(theta_enc),
            d * std::sin(theta) * cos_phi + n * std::sin(theta_enc),
            d * std::sin(phi)};
  }

  /// @brief Return a unique id for a measurement
  [[nodiscard]] int Uid(int fid, int mid) const noexcept {
    return fid * cols + mid;
  }

  /// @brief Update camera info with this model
  void UpdateCameraInfo(sensor_msgs::CameraInfo& cinfo) const {
    cinfo.height = rows;
    cinfo.width = cols;
    cinfo.distortion_model = info.prod_line;

    // cinfo.D.reserve(altitudes.size() + azimuths.size());
    // cinfo.D.insert(cinfo.D.end(), altitudes.begin(), altitudes.end());
    // cinfo.D.insert(cinfo.D.end(), azimuths.begin(), azimuths.end());
    cinfo.D.reserve(pixel_shifts().size());
    cinfo.D.insert(cinfo.D.end(), pixel_shifts().begin(), pixel_shifts().end());

    cinfo.K[0] = dt_col;       // time between each column
    cinfo.K[1] = d_azimuth;    // radian between each column
    cinfo.K[2] = beam_offset;  // distance from center to beam
  }
};

/// @brief Stores data for a (sub)scan
struct LidarScan {
  int icol{0};   // column index
  int iscan{0};  // subscan index
  int prev_uid{-1};
  double min_range{};
  double max_range{};
  bool destagger{false};

  cv::Mat image;
  CloudT cloud;
  std::vector<uint64_t> times;  // all time stamps [nanosecond]

  [[nodiscard]] int rows() const noexcept { return image.rows; }
  [[nodiscard]] int cols() const noexcept { return image.cols; }

  /// @brief whether this scan is full
  [[nodiscard]] bool IsFull() const noexcept { return icol >= cols(); }

  /// @brief Starting column of this scan
  [[nodiscard]] int StartingCol() const noexcept { return iscan * cols(); }

  /// @brief Allocate storage for the scan
  void Allocate(int rows, int cols) {
    image.create(rows, cols, CV_32FC4);
    cloud = CloudT(cols, rows);  // point cloud ctor takes width and height
    times.resize(cols, 0);
  }

  /// @brief Detect if there is a jump in the lidar data
  /// @return 0 - no jump, >0 - jump forward in time, <0 - jump backward in time
  [[nodiscard]] int DetectJump(int uid) noexcept {
    int jump = 0;

    if (prev_uid >= 0) {
      // Ideally the increment should be 1 hence the jump should be 0
      jump = uid - prev_uid - 1;
    }

    prev_uid = uid;
    return jump;
  }

  /// @brief Hard reset internal counters and prev_uid
  void HardReset() noexcept {
    icol = 0;
    iscan = 0;
    prev_uid = -1;
  }

  /// @brief Try to reset the internal counters if it is full
  void SoftReset(int full_col) noexcept {
    // Reset col (usually to 0 but in the rare case that data jumps forward
    // it will be non-zero)
    icol = icol % image.cols;

    // Reset scan if we have a full sweep
    if (iscan * image.cols >= full_col) {
      iscan = 0;
    }
  }

  /// @brief Invalidate an entire column
  void InvalidateColumn(double dt_col) {
    for (int irow = 0; irow < static_cast<int>(cloud.height); ++irow) {
      auto& pt = cloud.at(icol, irow);
      pt.x = pt.y = pt.z = kFloatNaN;
    }

    for (int irow = 0; irow < image.rows; ++irow) {
      auto& px = image.at<cv::Vec4f>(irow, icol);
      px[0] = px[1] = px[2] = px[3] = kFloatNaN;
    }

    // It is possible that the jump spans two subscans, this will cause the
    // first timestamp to be wrong when we publish the data, therefore we need
    // to extrapolate timestamp here
    times.at(icol) = (icol == 0 ? times.back() : times.at(icol - 1)) +
                     static_cast<uint64_t>(dt_col * 1e9);

    // Move on to next column
    ++icol;
  }

  void DecodeColumn(const uint8_t* const col_buf, const LidarModel& model) {
    if (icol >= image.cols) {
      ROS_WARN_STREAM("Decode: " << icol << ">=" << image.cols);
      return;
    }

    const auto& pf = *model.pf;
    const uint64_t t_ns = pf.col_timestamp(col_buf);
    const uint32_t encoder = pf.col_encoder(col_buf);
    const uint32_t status = pf.col_status(col_buf);
    bool col_valid = (status == 0xffffffff);

    // Compute azimuth angle theta0, this should always be valid
    // const auto theta_enc = kTau - mid * model_.d_azimuth;
    const float theta_enc = kTau * (1.0f - encoder / 90112.0f);
    times.at(icol) = t_ns;

    for (int ipx = 0; ipx < pf.pixels_per_column; ++ipx) {
      const uint8_t* const px_buf = pf.nth_px(ipx, col_buf);
      const auto raw_range = pf.px_range(px_buf);
      const float range = raw_range * kMmToM;

      // im_col is where the pixel should go in the image
      // it is the same as icol when we are not in staggered mode
      int im_col = icol;
      if (destagger) {
        // add pixel shift to get where the pixel should be
        im_col += model.pixel_shifts()[ipx];
        // if it is outside the current subscan, we set this pixel invalid
        if (im_col < 0 || im_col >= cols()) {
          col_valid = false;
          // make sure index is within bound
          im_col = im_col % cols();
        }
      }

      // Set point
      auto& pt = cloud.at(icol, ipx);
      if (col_valid && min_range < range && range < max_range) {
        pt.getArray3fMap() = model.ToPoint(range, theta_enc, ipx);

        // https://github.com/ouster-lidar/ouster_example/issues/128
        // Intensity: whereas most "normal" surfaces lie in between 0 - 1000
        // Intensity-SLAM
        // https://arxiv.org/pdf/2102.03798.pdf
        // const float signal = static_cast<float>(pf.px_signal(px_buf));
        // const auto r2 = pt.getVector3fMap().squaredNorm();
        // pt.intensity = signal * r2;
        pt.intensity = static_cast<float>(pf.px_reflectivity(px_buf));
      } else {
        pt.x = pt.y = pt.z = pt.intensity = kFloatNaN;
      }

      // Set pixel
      auto& px = image.at<ImageData>(ipx, im_col);
      px.x = pt.x;
      px.y = pt.y;
      px.z = pt.z;
      px.r = std::hypot(pt.x, pt.y, pt.z);

      // pt.r = std::min<uint16_t>(pf.px_reflectivity(px_buf) >> 5, 255);
      // pt.g = std::min<uint16_t>(pf.px_signal(px_buf) >> 2, 255);
      // pt.b = std::min<uint16_t>(pf.px_ambient(px_buf), 255);
    }

    // Move on to next column
    ++icol;
  }

  /// @brief Update camera info roi data with this scan
  void UpdateRoi(sensor_msgs::RegionOfInterest& roi) const noexcept {
    // Update camera info roi with curr_scan
    roi.x_offset = StartingCol();
    roi.y_offset = 0;
    roi.width = image.cols;
    roi.height = image.rows;
    roi.do_rectify = destagger;
  }
};

/// @brief Decoder node
class Decoder {
 public:
  explicit Decoder(const ros::NodeHandle& pnh);

  // No copy no move
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;

  /// Callbacks
  void LidarPacketCb(const ouster_ros::PacketMsg& lidar_msg);
  void ImuPacketCb(const ouster_ros::PacketMsg& imu_msg);

 private:
  /// Initialize ros related stuff (frame, publisher, subscriber)
  void InitRos();
  /// Initialize ouster related stuff
  void InitOuster();
  /// Initialize all parameters
  void InitParams();
  /// Send static transforms
  void SendTransform();

  /// Whether we are still waiting for alignment to mid 0
  [[nodiscard]] bool CheckAlign(int mid);

  /// Publish messages
  void PublishAndReset();

  /// Record processing time of lidar callback, print warning if it exceeds time
  /// between two packets
  void Timing(const ros::Time& start) const;

  // ros
  ros::NodeHandle pnh_;
  image_transport::ImageTransport it_;
  ros::Subscriber lidar_sub_, imu_sub_;
  ros::Publisher cloud_pub_, imu_pub_;
  ros::Publisher range_pub_;
  image_transport::CameraPublisher camera_pub_;
  tf2_ros::StaticTransformBroadcaster static_tf_;
  std::string sensor_frame_, lidar_frame_, imu_frame_;

  // data
  LidarModel model_;
  LidarScan scan_;
  sensor_msgs::CameraInfoPtr cinfo_msg_;

  // params
  double gravity_{};        // gravity
  bool strict_{false};      // strict mode will die if data jumps backwards
  bool need_align_{true};   // whether to align scan
  double acc_noise_var_{};  // discrete time acc noise variance
  double gyr_noise_var_{};  // discrete time gyr noise variance
};

Decoder::Decoder(const ros::NodeHandle& pnh) : pnh_(pnh), it_(pnh) {
  InitOuster();
  InitParams();
  InitRos();
  SendTransform();
}

void Decoder::InitOuster() {
  // Call service to retrieve sensor info, this must be done first
  ouster_ros::OSConfigSrv cfg{};
  auto client = pnh_.serviceClient<ouster_ros::OSConfigSrv>("os_config");
  client.waitForExistence();
  if (!client.call(cfg)) {
    throw std::runtime_error("Calling config service failed");
  }

  // parse metadata into lidar model
  model_ = LidarModel{cfg.response.metadata};
  ROS_INFO_STREAM("Lidar mode: " << os::to_string(model_.info.mode));
  ROS_INFO("Lidar: %d x %d @ %d hz", model_.rows, model_.cols, model_.freq);
  ROS_INFO("Columns per packet: %d", model_.pf->columns_per_packet);
  ROS_INFO("Pixels per column: %d", model_.pf->pixels_per_column);

  // Generate partial camera info message
  cinfo_msg_ = boost::make_shared<sensor_msgs::CameraInfo>();
  model_.UpdateCameraInfo(*cinfo_msg_);
}

void Decoder::InitParams() {
  strict_ = pnh_.param<bool>("strict", false);
  ROS_INFO("Strict: %s", strict_ ? "true" : "false");
  gravity_ = pnh_.param<double>("gravity", kDefaultGravity);
  ROS_INFO("Gravity: %f", gravity_);
  scan_.destagger = pnh_.param<bool>("destagger", false);
  ROS_INFO("Destagger: %s", scan_.destagger ? "true" : "false");
  scan_.min_range = pnh_.param<double>("min_range", 0.5);
  ROS_INFO("Min range: %f", scan_.min_range);
  scan_.max_range = pnh_.param<double>("max_range", 128.0);
  ROS_INFO("Min range: %f", scan_.max_range);

  int num_subscans = pnh_.param<int>("divide", 1);
  // Make sure cols is divisible by num_subscans
  if (num_subscans < 1 || model_.cols % num_subscans != 0) {
    throw std::runtime_error("num subscans is not divisible by cols: " +
                             std::to_string(model_.cols) + " / " +
                             std::to_string(num_subscans));
  }

  const int scan_cols = model_.cols / num_subscans;
  ROS_INFO("Subscan %d x %d, total %d", model_.rows, scan_cols, num_subscans);
  scan_.Allocate(model_.rows, scan_cols);

  acc_noise_var_ = pnh_.param<double>("acc_noise_std", 0.0023);
  gyr_noise_var_ = pnh_.param<double>("gyr_noise_std", 0.00026);
  // https://github.com/ethz-asl/kalibr/wiki/IMU-Noise-Model
  acc_noise_var_ = std::pow(acc_noise_var_, 2) * 100.0;
  gyr_noise_var_ = std::pow(gyr_noise_var_, 2) * 100.0;
  ROS_INFO("Discrete time acc noise var: %f, gyr nosie var: %f",
           acc_noise_var_,
           gyr_noise_var_);
}

void Decoder::InitRos() {
  // Subscribers, queue size is 1 second
  lidar_sub_ =
      pnh_.subscribe("lidar_packets", 640, &Decoder::LidarPacketCb, this);
  imu_sub_ = pnh_.subscribe("imu_packets", 100, &Decoder::ImuPacketCb, this);
  ROS_INFO_STREAM("Subscribing lidar packets from: " << lidar_sub_.getTopic());
  ROS_INFO_STREAM("Subscribing imu packets from: " << imu_sub_.getTopic());

  // Publishers
  cloud_pub_ = pnh_.advertise<CloudT>("cloud", 10);
  camera_pub_ = it_.advertiseCamera("image", 10);
  imu_pub_ = pnh_.advertise<sensor_msgs::Imu>("imu", 100);
  range_pub_ = pnh_.advertise<sensor_msgs::Image>("range", 1);

  // Frames
  sensor_frame_ = pnh_.param<std::string>("sensor_frame", "os_sensor");
  lidar_frame_ = pnh_.param<std::string>("lidar_frame", "os_lidar");
  imu_frame_ = pnh_.param<std::string>("imu_frame", "os_imu");
  ROS_INFO_STREAM("Sensor frame: " << sensor_frame_);
  ROS_INFO_STREAM("Lidar frame: " << lidar_frame_);
  ROS_INFO_STREAM("Imu frame: " << imu_frame_);
}

void Decoder::SendTransform() {
  static_tf_.sendTransform(ouster_ros::transform_to_tf_msg(
      model_.info.imu_to_sensor_transform, sensor_frame_, imu_frame_));
  static_tf_.sendTransform(ouster_ros::transform_to_tf_msg(
      model_.info.lidar_to_sensor_transform, sensor_frame_, lidar_frame_));
}

void Decoder::PublishAndReset() {
  std_msgs::Header header;
  header.frame_id = lidar_frame_;
  header.stamp.fromNSec(scan_.times.back());  // use time of the last column

  // Publish image and camera_info
  // cinfo stores information about the full sweep, while roi stores information
  // about the subscan
  const auto image_msg =
      cv_bridge::CvImage(header, "32FC4", scan_.image).toImageMsg();
  cinfo_msg_->header = header;
  // Update camera info roi with scan
  cinfo_msg_->binning_x = scan_.iscan;
  cinfo_msg_->binning_y = model_.cols / scan_.cols();
  scan_.UpdateRoi(cinfo_msg_->roi);
  camera_pub_.publish(image_msg, cinfo_msg_);

  // Publish range image on demand
  if (range_pub_.getNumSubscribers() > 0) {
    cv::Mat range;
    cv::extractChannel(scan_.image, range, 3);
    range_pub_.publish(cv_bridge::CvImage(header, "32FC1", range).toImageMsg());
  }

  // Publish cloud
  pcl_conversions::toPCL(header, scan_.cloud.header);
  cloud_pub_.publish(scan_.cloud);

  // Increment subscan counter
  ++scan_.iscan;
  // Reset cached data after publish
  scan_.SoftReset(model_.cols);
  //  ROS_DEBUG("After reset, icol: %d, iscan: %d", scan_.icol, scan_.iscan);
}

void Decoder::Timing(const ros::Time& t_start) const {
  const auto t_end = ros::Time::now();
  const auto t_proc = (t_end - t_start).toSec();
  const auto ratio = t_proc / model_.dt_packet;
  if (ratio > 5) {
    ROS_WARN("Proc time: %f ms, meas time: %f ms, ratio: %f",
             t_proc * 1e3,
             model_.dt_packet * 1e3,
             ratio);
  }
  ROS_DEBUG_THROTTLE(1,
                     "Proc time: %f ms, meas time: %f ms, ratio: %f",
                     t_proc * 1e3,
                     model_.dt_packet * 1e3,
                     ratio);
}

bool Decoder::CheckAlign(int mid) {
  if (need_align_ && mid == 0) {
    need_align_ = false;
    ROS_INFO("Align start of scan to mid %d, icol in scan %d", mid, scan_.icol);
  }
  return need_align_;
}

void Decoder::LidarPacketCb(const ouster_ros::PacketMsg& lidar_msg) {
  const auto t0 = ros::Time::now();
  const auto* packet_buf = lidar_msg.buf.data();
  const auto& pf = *model_.pf;

  for (int col = 0; col < pf.columns_per_packet; ++col) {
    // Get column buffer
    const uint8_t* const col_buf = pf.nth_col(col, packet_buf);
    const int fid = pf.col_frame_id(col_buf);
    const int mid = pf.col_measurement_id(col_buf);

    // If we set need_align to true then this will wait for mid = 0 to
    // start a scan
    if (CheckAlign(mid)) {
      continue;
    }

    // The invariant here is that scan_.icol will always point at the current
    // column to be filled at the beginning of the loop

    // Sometimes the lidar packet will jump forward by a large chunk, we handle
    // this case here
    const auto jump = scan_.DetectJump(model_.Uid(fid, mid));
    if (jump == 0) {
      // Data arrived as expected, decode and forward
      scan_.DecodeColumn(col_buf, model_);
      if (scan_.IsFull()) {
        PublishAndReset();
        Timing(t0);
      }
    } else if (0 < jump && jump < model_.cols) {
      ROS_WARN("Packet jumped to f%d:m%d by %d columns.", fid, mid, jump);
      // Detect a jump, we need to forward scan icol by the same amount as jump
      // We could directly increment icol and publish if necessary, but
      // this will require us to zero the whole cloud at publish time which is
      // very time-consuming. Therefore, we choose to advance icol one by one
      // and zero out each column in the point cloud
      for (int i = 0; i < jump; ++i) {
        // zero cloud column at current col and then increment
        scan_.InvalidateColumn(model_.dt_col);
        // It is possible that this jump will span two scans, so if that is
        // the case, we need to publish the previous scan before moving forward
        if (scan_.IsFull()) {
          ROS_WARN("Jumped into a new scan, need to publish the previous one");
          PublishAndReset();
          Timing(t0);
        }
      }
    } else {
      ROS_ERROR("Packet jumped to f%d:m%d by %d columns.", fid, mid, jump);
      if (strict_) {
        ROS_FATAL("In strict mode, shutting down...");
        ros::shutdown();
      } else {
        ROS_WARN("Not in strict mode, reset internal state and wait for align");
        need_align_ = true;
        scan_.HardReset();
      }
      return;
    }
  }
}

void Decoder::ImuPacketCb(const ouster_ros::PacketMsg& imu_msg) {
  const auto* buf = imu_msg.buf.data();
  const auto& pf = *model_.pf;

  sensor_msgs::Imu m;
  m.header.stamp.fromNSec(pf.imu_gyro_ts(buf));
  m.header.frame_id = imu_frame_;

  // Invalidate orientation data since we don't have it
  // http://docs.ros.org/en/noetic/api/sensor_msgs/html/msg/Imu.html
  auto& q = m.orientation;
  q.x = q.y = q.z = q.w = 0;
  m.orientation_covariance[0] = -1;

  auto& a = m.linear_acceleration;
  a.x = pf.imu_la_x(buf) * gravity_;
  a.y = pf.imu_la_y(buf) * gravity_;
  a.z = pf.imu_la_z(buf) * gravity_;

  auto& w = m.angular_velocity;
  w.x = Deg2Rad(pf.imu_av_x(buf));
  w.y = Deg2Rad(pf.imu_av_y(buf));
  w.z = Deg2Rad(pf.imu_av_z(buf));

  for (int i = 0; i < 9; i += 4) {
    m.linear_acceleration_covariance[i] = acc_noise_var_;
    m.angular_velocity_covariance[i] = gyr_noise_var_;
  }

  imu_pub_.publish(m);
}

}  // namespace ouster_decoder

int main(int argc, char** argv) {
  ros::init(argc, argv, "os_decoder");

  ouster_decoder::Decoder node(ros::NodeHandle("~"));
  ros::spin();

  return 0;
}
