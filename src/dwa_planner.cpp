#include "dwa_planner_ros/dwa_planner.h"
#include <Eigen/Core>
#include <cmath>

namespace dwa_planner_ros {

DWAPlanner::DWAPlanner(base_local_planner::CostmapModel* costmap_model,
                       const std::vector<geometry_msgs::Point>& footprint_spec, double inscribed_radius,
                       double circumscribed_radius, ros::NodeHandle& nh)
  : costmap_model_(costmap_model)
  , footprint_spec_(footprint_spec)
  , inscribed_radius_(inscribed_radius)
  , circumscribed_radius_(circumscribed_radius)
{
  candidate_paths_pub_ = nh.advertise<nav_msgs::Path>("dwa_candidate_paths", 1);
  nh.param("map_frame", map_frame_, std::string("map"));
  nh.param("max_vel_x", max_vel_x_, 0.55);
  nh.param("min_vel_x", min_vel_x_, 0.0);
  nh.param("max_vel_theta", max_vel_theta_, 2.5);
  nh.param("min_vel_theta", min_vel_theta_, -2.5);
  nh.param("acc_lim_x", acc_lim_x_, 0.25);
  nh.param("acc_lim_theta", acc_lim_theta_, 1.2);
  nh.param("control_period", control_period_, 0.2);
  nh.param("path_distance_bias", path_distance_bias_, 32.0);
  nh.param("goal_distance_bias", goal_distance_bias_, 40.0);
  nh.param("occdist_scale", occdist_scale_, 0.01);
  nh.param("sim_time_samples", sim_time_samples_, 10);
  nh.param("vx_samples", vx_samples_, 10);
  nh.param("vth_samples", vth_samples_, 20);
}

DWAPlanner::~DWAPlanner() {}

bool DWAPlanner::computeVelocityCommands(const double& robot_vel_x, const double& robot_vel_theta,
                                         const double& robot_pose_x, const double& robot_pose_y, const double& robot_pose_theta,
                                         const std::vector<std::vector<double>>& global_plan, unsigned char const* const* costmap,
                                         int size_x, int size_y, double resolution, double origin_x, double origin_y,
                                         double& cmd_vel_x, double& cmd_vel_theta, const std::vector<geometry_msgs::PoseStamped> transformed_plan, costmap_2d::Costmap2DROS* costmap_ros)
{
  std::vector<std::pair<double, double>> sample_vels;
  if (!samplePotentialVels(robot_vel_x, robot_vel_theta, sample_vels)){
    return false;
  }

  std::vector<std::vector<double>> pruned_global_plan = cutGlobalPlan(global_plan, size_x, size_y, robot_pose_x, robot_pose_y);
  std::vector<std::pair<double, double>>::iterator it = sample_vels.begin();
  std::vector<std::vector<std::vector<double>>> path_all;
  double min_score = 1e10;

  while (it != sample_vels.end()) {
    std::vector<std::vector<double>> traj;
    costmap_ros_ = costmap_ros;
    generateTrajectory(robot_vel_x, robot_vel_theta, robot_pose_x, robot_pose_y, robot_pose_theta, it->first, it->second, traj, transformed_plan, costmap_ros_);

    if (!isPathFeasible(traj)) {
      ++it;
      continue;
    }

    double score = scoreTrajectory(traj, size_x, size_y, resolution, origin_x, origin_y, pruned_global_plan, costmap);
    if (score < min_score) {
      min_score = score;
      cmd_vel_x = it->first;
      cmd_vel_theta = it->second;
    }

    path_all.push_back(traj);
    ++it;
  }

  publishCandidatePaths(path_all);
  return true;
}

std::vector<std::vector<double>> DWAPlanner::cutGlobalPlan(const std::vector<std::vector<double>>& global_plan,
                                                           const int& size_x, const int& size_y, const double& robot_pose_x, const double& robot_pose_y)
{
  std::vector<std::vector<double>> pruned_global_plan;
  for (auto it = global_plan.begin(); it != global_plan.end(); ++it) {
    if (std::abs(it->at(0) - robot_pose_x) > size_x / 2.0 && std::abs(it->at(1) - robot_pose_y) > size_y / 2.0) {
      break;
    }
    pruned_global_plan.push_back(*it);
  }
  return pruned_global_plan;
}

double DWAPlanner::scoreTrajectory(const std::vector<std::vector<double>>& traj, const int& size_x, const int& size_y,
                                   const double& resolution, const double& origin_x, const double& origin_y,
                                   const std::vector<std::vector<double>>& global_plan, unsigned char const* const* costmap)
{
  int mx, my;
  int occupy = 0;
  for (auto it = traj.begin(); it != traj.end(); ++it) {
    worldToMap(it->at(0), it->at(1), mx, my, resolution, origin_x, origin_y);
    occupy = std::max(costmap[mx][my] - 0, occupy);
  }

  const std::vector<double>& end_pose = traj.back();
  const std::vector<double>& local_end_pose = global_plan.back();
  double dis2end = std::sqrt(std::pow(end_pose[0] - local_end_pose[0], 2) + std::pow(end_pose[1] - local_end_pose[1], 2));

  double dis2path = 1e10;
  for (auto it = global_plan.begin(); it != global_plan.end(); ++it) {
    dis2path = std::min(std::sqrt(std::pow(end_pose[0] - it->at(0), 2) + std::pow(end_pose[1] - it->at(1), 2)), dis2path);
  }

  return occdist_scale_ * occupy + goal_distance_bias_ * dis2end + path_distance_bias_ * dis2path;
}

void DWAPlanner::generateTrajectory(const double& robot_vel_x, const double& robot_vel_theta,
                                    const double& robot_pose_x, const double& robot_pose_y, const double& robot_pose_theta,
                                    const double& sample_vel_x, const double& sample_vel_theta, std::vector<std::vector<double>>& traj, 
                                    const std::vector<geometry_msgs::PoseStamped>transformed_plan, costmap_2d::Costmap2DROS* costmap_ros_)
{
  double pose_x = robot_pose_x;
  double pose_y = robot_pose_y;
  double pose_theta = robot_pose_theta;
  double vel_x = robot_vel_x;
  double vel_theta = robot_vel_theta;
  unsigned int start_mx, start_my, goal_mx, goal_my;
  geometry_msgs::PoseStamped current_pose_;
  geometry_msgs::PoseStamped goal = transformed_plan.back();

  costmap_ros_->getRobotPose(current_pose_);   
  double start_wx = current_pose_.pose.position.x;
  double start_wy = current_pose_.pose.position.y;
  double goal_wx = goal.pose.position.x;
  double goal_wy = goal.pose.position.y;

  if (!costmap_->worldToMap(start_wx, start_wy, start_mx, start_my) ||
      !costmap_->worldToMap(goal_wx, goal_wy, goal_mx, goal_my)) {
      ROS_WARN("Cannot convert world coordinates to map coordinates");
      }
  
  for (int i = 0; i < sim_time_samples_; ++i) {
    vel_x = computeNewLinearVelocities(sample_vel_x, vel_x, acc_lim_x_);
    vel_theta = computeNewAngularVelocities(sample_vel_theta, vel_theta, acc_lim_theta_, start_mx, start_my, goal_mx, goal_my);
    computeNewPose(pose_x, pose_y, pose_theta, vel_x, vel_theta);
    traj.push_back({pose_x, pose_y, pose_theta});
  }
}

void DWAPlanner::worldToMap(const double wx, const double wy, int& mx, int& my, const double resolution, const double origin_x, const double origin_y){
  mx = int((wx-origin_x)/resolution);
  my = int((wy-origin_y)/resolution);
}

void DWAPlanner::computeNewPose(double& pose_x, double& pose_y, double& pose_theta,
                                const double& vel_x, const double& vel_theta)
{
  pose_x += vel_x * std::cos(pose_theta) * control_period_;
  pose_y += vel_x * std::sin(pose_theta) * control_period_;
  pose_theta += vel_theta * control_period_;
}

double DWAPlanner::computeNewLinearVelocities(const double& target_vel, const double& current_vel, const double& acc_lim)
{
  if (target_vel < current_vel) {
    return std::max(target_vel, current_vel - acc_lim * control_period_);
  } else {
    return std::min(target_vel, current_vel + acc_lim * control_period_);
  }
}

double DWAPlanner::computeNewAngularVelocities(const double& target_vel, const double& current_vel, const double& acc_lim, 
                                        unsigned int start_mx, unsigned int start_my, unsigned int goal_mx, unsigned int goal_my)
{
    // Perform Bresenham's line algorithm to check for obstacles along the straight path
    std::vector<std::pair<int, int>> line_points = bresenhamLine(start_mx, start_my, goal_mx, goal_my);

    bool obstacle_found = false;
    for (const auto& point : line_points) {
        unsigned int mx = point.first;
        unsigned int my = point.second;

        // Get cost from the costmap at each point
        unsigned char cost = costmap_->getCost(mx, my);

        // Check if there's a lethal obstacle
        if (cost == costmap_2d::LETHAL_OBSTACLE) {
            obstacle_found = true;
            break;
        }
        if(cost == costmap_2d::FREE_SPACE){
          obstacle_found = false;
        }
    }

    if (obstacle_found) {
        if(target_vel < current_vel){
         return std::max(target_vel, current_vel - acc_lim * control_period_);
        }else{
          return std::min(target_vel, current_vel + acc_lim * control_period_);
        }
    } else{
        if(target_vel < current_vel){
         return std::max(target_vel, current_vel - acc_lim * control_period_)*1.5;
        }else{
          return std::min(target_vel, current_vel + acc_lim * control_period_)*1.5;
        }
    }
}

bool DWAPlanner::samplePotentialVels(const double& robot_vel_x, const double& robot_vel_theta,
                                     std::vector<std::pair<double, double>>& sample_vels)
{
  double min_vel_x = std::max(min_vel_x_, robot_vel_x - acc_lim_x_ * control_period_);
  double max_vel_x = std::min(max_vel_x_, robot_vel_x + acc_lim_x_ * control_period_);
  double min_vel_theta = std::max(min_vel_theta_, robot_vel_theta - acc_lim_theta_ * control_period_);
  double max_vel_theta = std::min(max_vel_theta_, robot_vel_theta + acc_lim_theta_ * control_period_);

  for (double v = min_vel_x; v <= max_vel_x; v += (max_vel_x - min_vel_x) / vx_samples_) {
    for (double w = min_vel_theta; w <= max_vel_theta; w += (max_vel_theta - min_vel_theta) / vth_samples_) {
      sample_vels.push_back(std::make_pair(v, w));
    }
  }

  return !sample_vels.empty();
}

bool DWAPlanner::isPathFeasible(const std::vector<std::vector<double>>& path)
{
  int look_ahead_idx = path.size() - 1;

  for (int i = 0; i <= look_ahead_idx; ++i) {
    if (costmap_model_->footprintCost(path[i][0], path[i][1], path[i][2], footprint_spec_, inscribed_radius_, circumscribed_radius_) == -1) {
      return false;
    }
  }

  return true;
}

void DWAPlanner::publishCandidatePaths(const std::vector<std::vector<std::vector<double>>>& candidate_paths)
{
  nav_msgs::Path gui_path;
  gui_path.header.frame_id = map_frame_;
  gui_path.header.stamp = ros::Time::now();
  geometry_msgs::PoseStamped pose;

  for (int i = 0; i < (int)candidate_paths.size(); i++)
  {
    for (int j = 0; j < (int)candidate_paths[i].size(); j++)
    {
      double x = candidate_paths[i][j][0];  // x position
      double y = candidate_paths[i][j][1];  // y position
      double theta = candidate_paths[i][j][2];  // orientation (theta)
      
      pose.pose.position.x = x;
      pose.pose.position.y = y;
      pose.pose.orientation = tf::createQuaternionMsgFromYaw(theta);
      gui_path.poses.push_back(pose);
    }

    for (int j = (int)candidate_paths[i].size() - 1; j >= 0; j--)
    {
      double x = candidate_paths[i][j][0];  // x position
      double y = candidate_paths[i][j][1];  // y position
      double theta = candidate_paths[i][j][2];  // orientation (theta)

      pose.pose.position.x = x;
      pose.pose.position.y = y;
      pose.pose.orientation = tf::createQuaternionMsgFromYaw(theta);
      gui_path.poses.push_back(pose);
    }
  }

  candidate_paths_pub_.publish(gui_path);
}

std::vector<std::pair<int, int>> DWAPlanner::bresenhamLine(int x0, int y0, int x1, int y1) {
        std::vector<std::pair<int, int>> points;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = (dx > dy ? dx : -dy) / 2, e2;

        while (true) {
            points.push_back(std::make_pair(x0, y0));
            if (x0 == x1 && y0 == y1) break;
            e2 = err;
            if (e2 > -dx) { err -= dy; x0 += sx; }
            if (e2 < dy) { err += dx; y0 += sy; }
        }
        return points;
    }

}
