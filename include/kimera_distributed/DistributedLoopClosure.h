/*
 * Copyright Notes
 *
 * Authors: Yulun Tian (yulun@mit.edu) Yun Chang (yunchang@mit.edu)
 */

#pragma once

#include <ros/ros.h>
#include <ros/time.h>

#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <pose_graph_tools/VLCFrames.h>
#include <pose_graph_tools/VLCRequests.h>

#include <kimera_multi_lcd/LoopClosureDetector.h>

#include <kimera_distributed/utils.h>

namespace lcd = kimera_multi_lcd;

namespace kimera_distributed {

class DistributedLoopClosure {
 public:
  DistributedLoopClosure(const ros::NodeHandle& n);
  ~DistributedLoopClosure();

  inline void getLoopClosures(std::vector<lcd::VLCEdge>* loop_closures) {
    *loop_closures = loop_closures_;
  }

  inline size_t getRobotId() const { return my_id_; }

  // For debugging purpose
  void saveLoopClosuresToFile(const std::string filename);

 private:
  ros::NodeHandle nh_;
  size_t my_id_;
  size_t num_robots_;

  bool log_output_;
  std::string log_output_dir_;
  std::atomic<bool> should_shutdown_{false};

  std::vector<size_t> received_bow_bytes_;
  std::vector<size_t> received_vlc_bytes_;

  // Loop closure detector
  std::shared_ptr<lcd::LoopClosureDetector> lcd_;
  lcd::LcdParams lcd_params_;
  std::mutex lcd_mutex_;

  // Loop closures
  std::vector<lcd::VLCEdge> loop_closures_;

  // List of potential loop closures
  // that require to request VLC frames
  std::unordered_map<size_t, std::vector<lcd::PotentialVLCEdge> > candidate_lc_;
  std::mutex candidate_lc_mutex_;
  std::queue<lcd::PotentialVLCEdge> queued_lc_;

  std::mutex vlc_service_mutex_;

  // Maximum number of VLC frames per robot to request in one batch
  int vlc_batch_size_;

  // Map from robot ID to name
  std::map<size_t, std::string> robot_names_;

  // ROS subscriber
  std::vector<ros::Subscriber> bow_sub_;
  std::vector<ros::Subscriber> vlc_requests_sub_;
  std::vector<ros::Subscriber> vlc_responses_sub_;

  // ROS publisher
  ros::Publisher loop_closure_pub_;
  ros::Publisher vlc_responses_pub_;
  ros::Publisher vlc_requests_pub_;

  // Threads
  std::unique_ptr<std::thread> verification_thread_;
  std::unique_ptr<std::thread> comms_thread_;

 private:
  /**
   * Run Geometric verification spin
   */
  void runVerification();

  /**
   * Run Comms spin to send requests
   */
  void runComms();

  /**
   * Callback to process bag of word vectors received from robots
   */
  void bowCallback(const pose_graph_tools::BowQueryConstPtr& msg);

  /**
   * Callback to process the VLC responses to our requests
   */
  void vlcResponsesCallback(const pose_graph_tools::VLCFramesConstPtr& msg);

  /**
   * Callback to process the VLC requests from other robots
   */
  void vlcRequestsCallback(const pose_graph_tools::VLCRequestsConstPtr& msg);

  /**
   * Publish detected loop closure
   */
  void publishLoopClosure(const lcd::VLCEdge& loop_closure_edge);

  /**
   * Publish VLC requests
   */
  void processVLCRequests(const size_t& robot_id,
                          const lcd::RobotPoseIdSet& vertex_ids);

  /**
   * Publish VLC Frame requests from other robots
   */
  void publishVLCRequests(const size_t& robot_id,
                          const lcd::RobotPoseIdSet& vertex_ids);

  /**
   * Request local VLC Frames from Kimera-VIO-ROS
   */
  bool requestVLCFrameService(const lcd::RobotPoseIdSet& vertex_ids);

  /**
   * Check and submit VLC requests
   */
  void requestFrames();

  /**
   * Perform geometric verification
   */
  void verifyLoopCallback();

  /**
   * Update the candidate list and verification queue
   */
  size_t updateCandidateList();

  /**
   * Log communication stats to file
   */
  void logCommStat(const std::string& filename);
};

}  // namespace kimera_distributed