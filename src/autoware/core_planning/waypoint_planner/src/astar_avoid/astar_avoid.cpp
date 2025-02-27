/*
 * Copyright 2015-2019 Autoware Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "waypoint_planner/astar_avoid/astar_avoid.h"

AstarAvoid::AstarAvoid()
  : nh_()
  , private_nh_("~")
  , closest_waypoint_index_(-1)
  , obstacle_waypoint_index_(-1)
  , closest_local_index_(-1)
  , costmap_initialized_(false)
  , current_pose_initialized_(false)
  , current_velocity_initialized_(false)
  , base_waypoints_initialized_(false)
  , closest_waypoint_initialized_(false)
  , terminate_thread_(false)
{
  private_nh_.param<int>("safety_waypoints_size", safety_waypoints_size_, 100);
  private_nh_.param<double>("update_rate", update_rate_, 10.0);

  private_nh_.param<bool>("enable_avoidance", enable_avoidance_, true);
  private_nh_.param<double>("avoid_waypoints_velocity", avoid_waypoints_velocity_, 4.0);
  private_nh_.param<double>("avoid_start_velocity", avoid_start_velocity_, 1.0);//km/h
  private_nh_.param<double>("replan_interval", replan_interval_, 0.5);
  private_nh_.param<int>("search_waypoints_size", search_waypoints_size_, 50);
  private_nh_.param<int>("search_waypoints_delta", search_waypoints_delta_, 2);
  private_nh_.param<int>("closest_search_size", closest_search_size_, 30);

  safety_waypoints_pub_ = nh_.advertise<autoware_msgs::Lane>("safety_waypoints", 1, true);
  costmap_sub_ = nh_.subscribe("costmap", 1, &AstarAvoid::costmapCallback, this);
  current_pose_sub_ = nh_.subscribe("current_pose", 1, &AstarAvoid::currentPoseCallback, this);
  current_velocity_sub_ = nh_.subscribe("current_velocity", 1, &AstarAvoid::currentVelocityCallback, this);
  base_waypoints_sub_ = nh_.subscribe("base_waypoints", 1, &AstarAvoid::baseWaypointsCallback, this);
  closest_waypoint_sub_ = nh_.subscribe("closest_waypoint", 1, &AstarAvoid::closestWaypointCallback, this);
  //谁发出来的
  obstacle_waypoint_sub_ = nh_.subscribe("obstacle_waypoint", 1, &AstarAvoid::obstacleWaypointCallback, this);

  rate_ = new ros::Rate(update_rate_);
}

AstarAvoid::~AstarAvoid()
{
  publish_thread_.join();
}

void AstarAvoid::costmapCallback(const nav_msgs::OccupancyGrid& msg)
{
  costmap_ = msg;
  tf::poseMsgToTF(costmap_.info.origin, local2costmap_);
  costmap_initialized_ = true;
}

void AstarAvoid::currentPoseCallback(const geometry_msgs::PoseStamped& msg)
{
  current_pose_global_ = msg;

  if (!enable_avoidance_)
  {
    current_pose_initialized_ = true;
  }
  else//使能避撞模式下
  {
    current_pose_local_.pose = transformPose(
        current_pose_global_.pose, getTransform(costmap_.header.frame_id, current_pose_global_.header.frame_id));
    current_pose_local_.header.frame_id = costmap_.header.frame_id;
    current_pose_local_.header.stamp = current_pose_global_.header.stamp;
    current_pose_initialized_ = true;
  }
}

void AstarAvoid::currentVelocityCallback(const geometry_msgs::TwistStamped& msg)
{
  current_velocity_ = msg;
  current_velocity_initialized_ = true;
}

void AstarAvoid::baseWaypointsCallback(const autoware_msgs::Lane& msg)
{
  static autoware_msgs::Lane prev_base_waypoints;
  base_waypoints_ = msg;

  if (base_waypoints_initialized_)
  {
    // detect waypoint change by timestamp update
    ros::Time t1 = prev_base_waypoints.header.stamp;
    ros::Time t2 = base_waypoints_.header.stamp;
    if ((t2 - t1).toSec() > 1e-3)
    {
      ROS_INFO("Receive new /base_waypoints, reset waypoint index.");
      closest_local_index_ = -1; // reset local closest waypoint
      prev_base_waypoints = base_waypoints_;
    }
  }
  else
  {
    prev_base_waypoints = base_waypoints_;
  }

  base_waypoints_initialized_ = true;
}

void AstarAvoid::closestWaypointCallback(const std_msgs::Int32& msg)
{
  closest_waypoint_index_ = msg.data;

  if (closest_waypoint_index_ == -1)
  {
    closest_local_index_ = -1; // reset local closest waypoint
  }

  closest_waypoint_initialized_ = true;
}

void AstarAvoid::obstacleWaypointCallback(const std_msgs::Int32& msg)
{
  obstacle_waypoint_index_ = msg.data;
}

void AstarAvoid::run()
{
  // check topics
  state_ = AstarAvoid::STATE::INITIALIZING;

  while (ros::ok())
  {
    ros::spinOnce();
    if (checkInitialized())
    {
      break;
    }
    ROS_WARN("Waiting for subscribing topics...");
    ros::Duration(1.0).sleep();
  }

  // main loop 时钟时间
  int end_of_avoid_index = -1;//初始化用于终止发布路径点的索引
  ros::WallTime start_plan_time = ros::WallTime::now();//设置规划开始的时间。
  ros::WallTime start_avoid_time = ros::WallTime::now();

  // reset obstacle index
  obstacle_waypoint_index_ = -1;//初始化用于表示障碍物的索引

  // relaying mode at startup
  state_ = AstarAvoid::STATE::RELAYING;
  // start publish thread
  //什么是线程？
  publish_thread_ = std::thread(&AstarAvoid::publishWaypoints, this);//开启第二个线程。

  while (ros::ok())
  {
    ros::spinOnce();

    // relay mode就会一直在这个循环中进行
    if (!enable_avoidance_)
    {
      rate_->sleep();
      continue;
    }

    // avoidance mode
    bool found_obstacle = (obstacle_waypoint_index_ >= 0);
    //avoid_start_velocity_避让开始时自车速度4m/s
    bool avoid_velocity = (current_velocity_.twist.linear.x < avoid_start_velocity_ / 3.6);

    // update state
    if (state_ == AstarAvoid::STATE::RELAYING)
    {
      avoid_waypoints_ = base_waypoints_;

      if (found_obstacle)
      {
        ROS_INFO("RELAYING -> STOPPING, Decelerate for stopping");
        state_ = AstarAvoid::STATE::STOPPING;
      }
    }
    else if (state_ == AstarAvoid::STATE::STOPPING)
    {
      //检查重规划时间间隔，时间间隔越小越可能进行重规划replan_interval=2hz,每0.5s更新一次,toSec()把时间戳转化成浮点型格式
      bool replan = ((ros::WallTime::now() - start_plan_time).toSec() > replan_interval_);

      if (!found_obstacle)
      {
        //ROS_INFO("STOPPING -> RELAYING, Obstacle disappers");
        state_ = AstarAvoid::STATE::RELAYING;
      }
      //从停车模式转换为规划模式
      else if (replan && avoid_velocity)
      //else if (replan)
      {
        ROS_INFO("STOPPING -> PLANNING, Start A* planning");
        state_ = AstarAvoid::STATE::PLANNING;
      }
    }
    //确定避障路线并相应添加进avoid_waypoints_，同时更新传入函数的end_of_avoid_index，
    //避障路线规划成功则从规划模式转换为避障模式，否则切换为停车模式
    else if (state_ == AstarAvoid::STATE::PLANNING)
    {
      start_plan_time = ros::WallTime::now();

      if (planAvoidWaypoints(end_of_avoid_index))
      {
        ROS_INFO("PLANNING -> AVOIDING, Found path");
        state_ = AstarAvoid::STATE::AVOIDING;
        start_avoid_time = ros::WallTime::now();
        std::cout << "avoiding" << std::endl;
      }
      else
      {
        ROS_INFO("PLANNING -> STOPPING, Cannot find path");
        state_ = AstarAvoid::STATE::STOPPING;
      }
    }
    else if (state_ == AstarAvoid::STATE::AVOIDING)
    {
      
      //reached为函数getLocalClosestWaypoint获得距离最近轨迹点的下标
      bool reached = (getLocalClosestWaypoint(avoid_waypoints_, current_pose_global_.pose, closest_search_size_) > end_of_avoid_index);
      if (reached)
      {
        ROS_INFO("AVOIDING -> RELAYING, Reached goal");
        state_ = AstarAvoid::STATE::RELAYING;
      }
      else if (found_obstacle && avoid_velocity)
      {
        bool replan = ((ros::WallTime::now() - start_avoid_time).toSec() > replan_interval_);
        if (replan)
        {
          ROS_INFO("AVOIDING -> STOPPING, Abort avoiding");
          state_ = AstarAvoid::STATE::STOPPING;
        }
      }
    }

    //目的就是一个周期内的剩余时间均处于sleep状态
    rate_->sleep();
  }

  terminate_thread_ = true;
}

bool AstarAvoid::checkInitialized()
{
  bool initialized = false;

  // check for relay mode
  initialized = (current_pose_initialized_ && closest_waypoint_initialized_ && base_waypoints_initialized_ &&
                 (closest_waypoint_index_ >= 0));

  // check for avoidance mode, additionally
  if (enable_avoidance_)
  {
    initialized = (initialized && (current_velocity_initialized_ && costmap_initialized_));
  }

  return initialized;
}

//逐步更新目标位姿，并执行从当前位姿到目标位姿的基于A*算法的增量搜索，确定避障路线并相应添加进avoid_waypoints_
bool AstarAvoid::planAvoidWaypoints(int& end_of_avoid_index)
{
  bool found_path = false;
  //得到最近轨迹点
  int closest_waypoint_index = getLocalClosestWaypoint(avoid_waypoints_, current_pose_global_.pose, closest_search_size_);

  // update goal pose incrementally and execute A* search，
  //search_waypoints_delta_ 用于跳过轨迹点进行增量搜索，static_cast<int>强制将数据类型转换为整型
  for (int i = search_waypoints_delta_; i < static_cast<int>(search_waypoints_size_); i += search_waypoints_delta_)
  {
    // update goal index
    //ff修改为+5
    int goal_waypoint_index = closest_waypoint_index + obstacle_waypoint_index_ + i + 5;
    if (goal_waypoint_index >= static_cast<int>(avoid_waypoints_.waypoints.size()))
    {
      break;
    }

    // update goal pose
    goal_pose_global_ = avoid_waypoints_.waypoints[goal_waypoint_index].pose;
    goal_pose_local_.header = costmap_.header;
    //转换为新位姿
    goal_pose_local_.pose = transformPose(goal_pose_global_.pose,
                                          getTransform(costmap_.header.frame_id, goal_pose_global_.header.frame_id));

    // initialize costmap for A* search 这里非常重要，是整个A*搜索算法的核心
    astar_.initialize(costmap_);

    // execute astar search
    // ros::WallTime start = ros::WallTime::now();
    found_path = astar_.makePlan(current_pose_local_.pose, goal_pose_local_.pose);
    // ros::WallTime end = ros::WallTime::now();

    static ros::Publisher pub = nh_.advertise<nav_msgs::Path>("debug", 1, true);

    // ROS_INFO("Astar planning: %f [s], at index = %d", (end - start).toSec(), goal_waypoint_index);

    if (found_path)
    {
      //getPath函数获得astar_中的成员path_
      pub.publish(astar_.getPath());
      end_of_avoid_index = goal_waypoint_index;
      //将用于避撞的轨迹astar_.getPath()合并进avoid_waypoints_并更新end_of_avoid_index
      mergeAvoidWaypoints(astar_.getPath(), end_of_avoid_index);
      if (avoid_waypoints_.waypoints.size() > 0)
      {
        ROS_INFO("Found GOAL at index = %d", goal_waypoint_index);
        astar_.reset();
        return true;
      }
      else
      {
        found_path = false;
      }
    }
    //用于重置astar_中用于A*搜索的信息
    astar_.reset();
  }

  ROS_ERROR("Can't find goal...");
  return false;
}

void AstarAvoid::mergeAvoidWaypoints(const nav_msgs::Path& path, int& end_of_avoid_index)
{
  autoware_msgs::Lane current_waypoints = avoid_waypoints_;

  // reset
  std::lock_guard<std::mutex> lock(mutex_);
  avoid_waypoints_.waypoints.clear();

  // add waypoints before start index
  int closest_waypoint_index = getLocalClosestWaypoint(current_waypoints, current_pose_global_.pose, closest_search_size_);
  for (int i = 0; i < closest_waypoint_index; ++i)
  {
    avoid_waypoints_.waypoints.push_back(current_waypoints.waypoints.at(i));
  }

  // set waypoints for avoiding设置用于避撞的轨迹
  for (const auto& pose : path.poses)
  {
    autoware_msgs::Waypoint wp;
    wp.pose.header = avoid_waypoints_.header;
    wp.pose.pose = transformPose(pose.pose, getTransform(avoid_waypoints_.header.frame_id, pose.header.frame_id));
    wp.pose.pose.position.z = current_pose_global_.pose.position.z;  // height = const
    wp.twist.twist.linear.x = avoid_waypoints_velocity_ / 3.6;       // velocity = const
    avoid_waypoints_.waypoints.push_back(wp);
  }

  // add waypoints after goal index
  for (int i = end_of_avoid_index; i < static_cast<int>(current_waypoints.waypoints.size()); ++i)
  {
    avoid_waypoints_.waypoints.push_back(current_waypoints.waypoints.at(i));
  }

  // update index for merged waypoints
  end_of_avoid_index = closest_waypoint_index + path.poses.size();
}

void AstarAvoid::publishWaypoints()
{
  autoware_msgs::Lane current_waypoints;

  while (!terminate_thread_)
  {
    // select waypoints
    switch (state_)
    {
      case AstarAvoid::STATE::RELAYING:
        current_waypoints = base_waypoints_;
        break;
      case AstarAvoid::STATE::STOPPING:
        // do nothing, keep current waypoints
        break;
      case AstarAvoid::STATE::PLANNING:
        // do nothing, keep current waypoints
        break;
      case AstarAvoid::STATE::AVOIDING:
        current_waypoints = avoid_waypoints_;
        break;
      default:
        current_waypoints = base_waypoints_;
        break;
    }

    autoware_msgs::Lane safety_waypoints;
    safety_waypoints.header = current_waypoints.header;
    safety_waypoints.increment = current_waypoints.increment;

    // push waypoints from closest index
    for (int i = 0; i < safety_waypoints_size_; ++i)
    {
      int index = getLocalClosestWaypoint(current_waypoints, current_pose_global_.pose, closest_search_size_) + i;
      if (index < 0 || static_cast<int>(current_waypoints.waypoints.size()) <= index)
      {
        break;
      }
      const autoware_msgs::Waypoint& wp = current_waypoints.waypoints[index];
      safety_waypoints.waypoints.push_back(wp);
    }

    if (safety_waypoints.waypoints.size() > 0)
    {
      safety_waypoints_pub_.publish(safety_waypoints);
    }

    rate_->sleep();
  }
}

tf::Transform AstarAvoid::getTransform(const std::string& from, const std::string& to)
{
  tf::StampedTransform stf;
  try
  {
    tf_listener_.lookupTransform(from, to, ros::Time(0), stf);
  }
  catch (tf::TransformException ex)
  {
    ROS_ERROR("%s", ex.what());
  }
  return stf;
}

//主要作用是获得waypoints中相对于pose的距离最近轨迹点的下标
int AstarAvoid::getLocalClosestWaypoint(const autoware_msgs::Lane& waypoints, const geometry_msgs::Pose& pose, const int& search_size)
{
  static autoware_msgs::Lane local_waypoints;  // around self-vehicle
  const int prev_index = closest_local_index_;

  // search in all waypoints if lane_select judges you're not on waypoints
  if (closest_local_index_ == -1)
  {
    closest_local_index_ = getClosestWaypoint(waypoints, pose);
  }
  // search in limited area based on prev_index
  else
  {
    // get neighborhood waypoints around prev_index
    int start_index = std::max(0, prev_index - search_size / 2);
    int end_index = std::min(prev_index + search_size / 2, (int)waypoints.waypoints.size());
    auto start_itr = waypoints.waypoints.begin() + start_index;
    auto end_itr = waypoints.waypoints.begin() + end_index;
    local_waypoints.waypoints = std::vector<autoware_msgs::Waypoint>(start_itr, end_itr);

    // get closest waypoint in neighborhood waypoints圈定waypoints内prev_index下标对应轨迹点周围的“邻居轨迹点”
    closest_local_index_ = start_index + getClosestWaypoint(local_waypoints, pose);
  }

  return closest_local_index_;
}
