/*
 * Copyright Notes
 *
 * Authors: Yulun Tian (yulun@mit.edu), Yun Chang (yunchang@mit.edu)
 */

#include <kimera_distributed/DistributedLoopClosure.h>
#include <kimera_distributed/DistributedPcm.h>
#include <kimera_distributed/types.h>
#include <ros/ros.h>

using namespace kimera_distributed;

int main(int argc, char** argv) {
  ros::init(argc, argv, "kimera_distributed_loop_closure_node");
  ros::NodeHandle nh;

  DistributedLoopClosure dlcd(nh);

  ros::spin();

  return 0;
}
