/*
 * Copyright Notes
 *
 * Authors: Yulun Tian (yulun@mit.edu)
 */

#include <kimera_distributed/DistributedLoopClosure.h>
#include <pose_graph_tools/PoseGraph.h>
#include <ros/console.h>
#include <ros/ros.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <opengv/point_cloud/PointCloudAdapter.hpp>
#include <opengv/relative_pose/CentralRelativeAdapter.hpp>
#include <opengv/sac/Ransac.hpp>
#include <opengv/sac_problems/point_cloud/PointCloudSacProblem.hpp>
#include <opengv/sac_problems/relative_pose/CentralRelativePoseSacProblem.hpp>
#include <string>

using RansacProblem =
    opengv::sac_problems::relative_pose::CentralRelativePoseSacProblem;
using Adapter = opengv::relative_pose::CentralRelativeAdapter;
using AdapterStereo = opengv::point_cloud::PointCloudAdapter;
using RansacProblemStereo =
    opengv::sac_problems::point_cloud::PointCloudSacProblem;

namespace kimera_distributed {

DistributedLoopClosure::DistributedLoopClosure(const ros::NodeHandle& n)
    : nh_(n) {
  int my_id_int = -1;
  int num_robots_int = -1;
  ros::param::get("~robot_id", my_id_int);
  ros::param::get("~num_robots", num_robots_int);
  assert(my_id_int >= 0);
  assert(num_robots_int > 0);
  my_id_ = my_id_int;
  num_robots_ = num_robots_int;
  next_pose_id_ = 0;

  // Initiate orb matcher
  orb_feature_matcher_ = cv::DescriptorMatcher::create(3);

  // Path to log outputs
  ros::param::get("~log_output_path", log_output_path_);

  // Visual place recognition params
  ros::param::get("~alpha", alpha_);
  ros::param::get("~dist_local", dist_local_);
  ros::param::get("~max_db_results", max_db_results_);
  ros::param::get("~base_nss_factor", base_nss_factor_);
  ros::param::get("~min_nss_factor", min_nss_factor_);

  // Geometric verification params
  ros::param::get("~lowe_ratio", lowe_ratio_);
  ros::param::get("~max_ransac_iterations", max_ransac_iterations_);
  ros::param::get("~ransac_threshold", ransac_threshold_);
  ros::param::get("~geometric_verification_min_inlier_count",
                  geometric_verification_min_inlier_count_);
  ros::param::get("~geometric_verification_min_inlier_percentage",
                  geometric_verification_min_inlier_percentage_);

  // Initialize bag-of-word database
  std::string orb_vocab_path;
  ros::param::get("~vocabulary_path", orb_vocab_path);
  OrbVocabulary vocab;
  vocab.load(orb_vocab_path);
  db_BoW_ = std::unique_ptr<OrbDatabase>(new OrbDatabase(vocab));
  shared_db_BoW_ = std::unique_ptr<OrbDatabase>(new OrbDatabase(vocab));

  // Subscriber
  for (size_t id = my_id_; id < num_robots_; ++id) {
    std::string topic =
        "/kimera" + std::to_string(id) + "/kimera_vio_ros/bow_query";
    ros::Subscriber sub =
        nh_.subscribe(topic, 10, &DistributedLoopClosure::bowCallback, this);
    bow_subscribers.push_back(sub);
  }

  // Publisher
  std::string loop_closure_topic =
      "/kimera" + std::to_string(my_id_) + "/kimera_distributed/loop_closure";
  loop_closure_publisher_ = nh_.advertise<pose_graph_tools::PoseGraphEdge>(
      loop_closure_topic, 10, false);

  ROS_INFO_STREAM("Distributed Kimera node initialized (ID = "
                  << my_id_ << "). \n"
                  << "Parameters: \n"
                  << "alpha = " << alpha_ << "\n"
                  << "dist_local = " << dist_local_ << "\n"
                  << "max_db_results = " << max_db_results_ << "\n"
                  << "base_nss_factor = " << base_nss_factor_ << "\n"
                  << "min_nss_factor = " << min_nss_factor_ << "\n"
                  << "lowe_ratio = " << lowe_ratio_ << "\n"
                  << "max_ransac_iterations = " << max_ransac_iterations_
                  << "\n"
                  << "ransac_threshold = " << ransac_threshold_ << "\n"
                  << "geometric_verification_min_inlier_count = "
                  << geometric_verification_min_inlier_count_ << "\n"
                  << "geometric_verification_min_inlier_percentage = "
                  << geometric_verification_min_inlier_percentage_);
}

DistributedLoopClosure::~DistributedLoopClosure() {}

void DistributedLoopClosure::bowCallback(
    const kimera_distributed::BowQueryConstPtr& msg) {
  RobotID robot_id = msg->robot_id;
  assert(robot_id >= my_id_);
  PoseID pose_id = msg->pose_id;
  VertexID vertex_query(robot_id, pose_id);
  DBoW2::BowVector bow_vec;
  BowVectorFromMsg(msg->bow_vector, &bow_vec);
  last_callback_time_ = ros::Time::now();

  VertexID vertex_match;
  // Detect loop closures with my trajectory
  if (detectLoopInMyDB(vertex_query, bow_vec, &vertex_match)) {
    gtsam::Pose3 T_query_match;
    if (recoverPose(vertex_query, vertex_match, &T_query_match)) {
      VLCEdge edge(vertex_query, vertex_match, T_query_match);
      loop_closures_.push_back(edge);
      publishLoopClosure(edge);  // Publish to pcm node
    }
  }

  // Detect loop closures with other robots' trajectories
  if (robot_id == my_id_) {
    if (detectLoopInSharedDB(vertex_query, bow_vec, &vertex_match)) {
      gtsam::Pose3 T_query_match;
      if (recoverPose(vertex_query, vertex_match, &T_query_match)) {
        VLCEdge edge(vertex_query, vertex_match, T_query_match);
        loop_closures_.push_back(edge);
        publishLoopClosure(edge);  // Publish to pcm node
      }
    }
  }

  // For debugging
  saveLoopClosuresToFile(log_output_path_ + "loop_closures.csv");

  // Add Bag-of-word vector to database
  if (robot_id == my_id_) {
    assert(pose_id == next_pose_id_);
    assert(db_BoW_->add(bow_vec) == next_pose_id_);
    latest_bowvec_ = bow_vec;
    next_pose_id_++;
  } else {
    uint32_t db_index = shared_db_BoW_->add(bow_vec);
    shared_db_to_vertex_[db_index] = vertex_query;
  }
}

bool DistributedLoopClosure::detectLoopInMyDB(
    const VertexID& vertex_query, const DBoW2::BowVector bow_vector_query,
    VertexID* vertex_match) {
  double nss_factor = base_nss_factor_;
  int max_possible_match_id = static_cast<int>(next_pose_id_) - 1;
  if (vertex_query.first == my_id_) {
    max_possible_match_id -= dist_local_;
    nss_factor =
        db_BoW_->getVocabulary()->score(bow_vector_query, latest_bowvec_);
    if (nss_factor < min_nss_factor_) {
      return false;
    }
  }
  if (max_possible_match_id < 0) max_possible_match_id = 0;

  DBoW2::QueryResults query_result;
  db_BoW_->query(bow_vector_query, query_result, max_db_results_,
                 max_possible_match_id);

  if (!query_result.empty()) {
    DBoW2::Result best_result = query_result[0];
    if (best_result.Score >= alpha_ * nss_factor) {
      *vertex_match = std::make_pair(my_id_, best_result.Id);
      return true;
    }
  }
  return false;
}

bool DistributedLoopClosure::detectLoopInSharedDB(
    const VertexID& vertex_query, const DBoW2::BowVector bow_vector_query,
    VertexID* vertex_match) {
  DBoW2::QueryResults query_result;
  shared_db_BoW_->query(bow_vector_query, query_result, max_db_results_);

  if (!query_result.empty()) {
    DBoW2::Result best_result = query_result[0];
    if (best_result.Score >= alpha_ * base_nss_factor_) {
      *vertex_match = shared_db_to_vertex_[best_result.Id];
      return true;
    }
  }
  return false;
}

void DistributedLoopClosure::requestVLCFrame(const VertexID& vertex_id) {
  if (vlc_frames_.find(vertex_id) != vlc_frames_.end()) {
    // Return if this frame already exists locally
    return;
  }
  RobotID robot_id = vertex_id.first;
  PoseID pose_id = vertex_id.second;
  std::string service_name =
      "/kimera" + std::to_string(robot_id) + "/kimera_vio_ros/vlc_frame_query";

  VLCFrameQuery query;
  query.request.robot_id = robot_id;
  query.request.pose_id = pose_id;
  if (!ros::service::call(service_name, query)) {
    ROS_ERROR_STREAM("Could not query VLC frame!");
  }

  VLCFrame frame;
  VLCFrameFromMsg(query.response.frame, &frame);
  assert(frame.robot_id_ == robot_id);
  assert(frame.pose_id_ == pose_id);

  vlc_frames_[vertex_id] = frame;
}

void DistributedLoopClosure::ComputeMatchedIndices(
    const VertexID& vertex_query, const VertexID& vertex_match,
    std::vector<unsigned int>* i_query,
    std::vector<unsigned int>* i_match) const {
  // Get two best matches between frame descriptors.
  std::vector<std::vector<cv::DMatch>> matches;

  VLCFrame frame_query = vlc_frames_.find(vertex_query)->second;
  VLCFrame frame_match = vlc_frames_.find(vertex_match)->second;

  orb_feature_matcher_->knnMatch(frame_query.descriptors_mat_,
                                 frame_match.descriptors_mat_, matches, 2u);

  for (const std::vector<cv::DMatch>& match : matches) {
    if (match.at(0).distance < lowe_ratio_ * match.at(1).distance) {
      i_query->push_back(match[0].queryIdx);
      i_match->push_back(match[0].trainIdx);
    }
  }
}

bool DistributedLoopClosure::recoverPose(const VertexID& vertex_query,
                                         const VertexID& vertex_match,
                                         gtsam::Pose3* T_query_match) {
  ROS_INFO_STREAM(
      "Checking loop closure between "
      << "(" << vertex_query.first << ", " << vertex_query.second << ")"
      << " and "
      << "(" << vertex_match.first << ", " << vertex_match.second << ")");

  requestVLCFrame(vertex_query);
  requestVLCFrame(vertex_match);

  // Find correspondences between frames.
  std::vector<unsigned int> i_query, i_match;
  ComputeMatchedIndices(vertex_query, vertex_match, &i_query, &i_match);
  assert(i_query.size() == i_match.size());

  opengv::points_t f_ref, f_cur;
  f_ref.resize(i_match.size());
  f_cur.resize(i_query.size());
  for (size_t i = 0; i < i_match.size(); i++) {
    f_cur[i] = (vlc_frames_[vertex_query].keypoints_.at(i_query[i]));
    f_ref[i] = (vlc_frames_[vertex_match].keypoints_.at(i_match[i]));
  }

  AdapterStereo adapter(f_ref, f_cur);

  // Compute transform using RANSAC 3-point method (Arun).
  std::shared_ptr<RansacProblemStereo> ptcloudproblem_ptr(
      new RansacProblemStereo(adapter, true));
  opengv::sac::Ransac<RansacProblemStereo> ransac;
  ransac.sac_model_ = ptcloudproblem_ptr;
  ransac.max_iterations_ = max_ransac_iterations_;
  ransac.threshold_ = ransac_threshold_;

  // Compute transformation via RANSAC.
  bool ransac_success = ransac.computeModel();

  if (ransac_success) {
    if (ransac.inliers_.size() < geometric_verification_min_inlier_count_) {
      ROS_INFO_STREAM("Number of inlier correspondences after RANSAC "
                      << ransac.inliers_.size() << " is too low.");
      return false;
    }

    double inlier_percentage =
        static_cast<double>(ransac.inliers_.size()) / f_ref.size();
    if (inlier_percentage < geometric_verification_min_inlier_percentage_) {
      ROS_INFO_STREAM("Percentage of inlier correspondences after RANSAC "
                      << inlier_percentage << " is too low.");
      return false;
    }

    opengv::transformation_t T = ransac.model_coefficients_;

    // Yulun: this is the relative pose from the query frame to the match frame?
    *T_query_match = gtsam::Pose3(gtsam::Rot3(T.block<3, 3>(0, 0)),
                                  gtsam::Point3(T(0, 3), T(1, 3), T(2, 3)));

    ROS_INFO_STREAM("Verified loop closure!");

    return true;
  }

  return false;
}

void DistributedLoopClosure::getLoopClosures(
    std::vector<VLCEdge>* loop_closures) {
  *loop_closures = loop_closures_;
}

void DistributedLoopClosure::saveLoopClosuresToFile(
    const std::string filename) {
  std::ofstream file;
  file.open(filename);

  std::vector<VLCEdge> loop_closures;
  getLoopClosures(&loop_closures);

  // file format
  file << "robot1,pose1,robot2,pose2,qx,qy,qz,qw,tx,ty,tz\n";

  for (size_t i = 0; i < loop_closures.size(); ++i) {
    VLCEdge edge = loop_closures[i];
    file << edge.vertex_src_.first << ",";
    file << edge.vertex_src_.second << ",";
    file << edge.vertex_dst_.first << ",";
    file << edge.vertex_dst_.second << ",";
    gtsam::Pose3 pose = edge.T_src_dst_;
    gtsam::Quaternion quat = pose.rotation().toQuaternion();
    gtsam::Point3 point = pose.translation();
    file << quat.x() << ",";
    file << quat.y() << ",";
    file << quat.z() << ",";
    file << quat.w() << ",";
    file << point.x() << ",";
    file << point.y() << ",";
    file << point.z() << "\n";
  }

  file.close();
}

void DistributedLoopClosure::publishLoopClosure(
    const VLCEdge& loop_closure_edge) {
  pose_graph_tools::PoseGraphEdge msg_edge;
  VLCEdgeToMsg(loop_closure_edge, &msg_edge);
  loop_closure_publisher_.publish(msg_edge);
}

}  // namespace kimera_distributed