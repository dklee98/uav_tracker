#ifndef REAL_IROS_H
#define REAL_IROS_H

#include "utility.h"

///// common headers for depth_to_pcl/octo and local_planner
#include <ros/ros.h>
#include <Eigen/Eigen> // whole Eigen library : Sparse(Linearalgebra) + Dense(Core+Geometry+LU+Cholesky+SVD+QR+Eigenvalues)
#include <iostream> //cout
#include <math.h> // pow
#include <chrono> 
#include <tf/LinearMath/Quaternion.h> // to Quaternion_to_euler
#include <tf/LinearMath/Matrix3x3.h> // to Quaternion_to_euler

///// headers for local_planner
#include <std_msgs/Bool.h>
#include <tf2_msgs/TFMessage.h> //for tf between frames
#include <geometry_msgs/Point.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Twist.h>
#include <visualization_msgs/Marker.h>
#include <nav_msgs/Path.h>

///// headers for pcl
#include <yolo_ros_simple/bbox.h>
#include <yolo_ros_simple/bboxes.h>
#include <sensor_msgs/PointCloud2.h>

#include <pcl/point_types.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/conversions.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/common/transforms.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <cv_bridge/cv_bridge.h>

using namespace std;
using namespace std::chrono; 
using namespace Eigen;

////////////////////////////////////////////////////////////////////////////////////////////////////


class ieee_uav_class{
  public:

    std::string m_depth_topic, m_depth_base, m_body_base, m_fixed_frame, m_bbox_out_topic; 
    bool m_bbox_check=false, m_depth_check=false, m_tf_check=false, m_body_t_cam_check=false;

    ///// for yolo and pcl
    cv_bridge::CvImagePtr m_depth_ptr;
    double m_scale_factor;
    geometry_msgs::Point m_detected_center;
    double m_pcl_max_range=0.0, m_f_x, m_f_y, m_c_x, m_c_y;
    pcl::PointCloud<pcl::PointXYZ> m_depth_cvt_pcl, m_depth_cvt_pcl_empty;

    //// states
    Matrix4f m_map_t_cam = Matrix4f::Identity();
    Matrix4f m_map_t_body = Matrix4f::Identity();
    Matrix4f m_body_t_cam = Matrix4f::Identity();
    double m_curr_roll=0.0, m_curr_pitch=0.0, m_curr_yaw=0.0, m_cvt_quat_x=0.0, m_cvt_quat_y=0.0, m_cvt_quat_z=0.0, m_cvt_quat_w=1.0;

    //// control
    double m_altitude_fixed;

    ///// ros and tf
    ros::NodeHandle nh;
    ros::Subscriber m_depth_sub;
    ros::Subscriber m_tf_sub;
    ros::Subscriber m_bbox_sub;
    ros::Publisher m_best_branch_pub;
    ros::Publisher m_detected_target_pcl_pub;

    ros::Timer m_octo_update_and_publisher;
    ros::Timer m_local_planner_thread;

    void depth_callback(const sensor_msgs::Image::ConstPtr& msg);
    void tf_callback(const tf2_msgs::TFMessage::ConstPtr& msg);
    void bbox_callback(const yolo_ros_simple::bboxes::ConstPtr& msg);

    ieee_uav_class(ros::NodeHandle& n) : nh(n){
      ROS_WARN("Class generating...");
      ///// params
      nh.param("/altitude_fixed", m_altitude_fixed, 0.8);
      nh.param("/pcl_max_range", m_pcl_max_range, 5.0);
      nh.param("/f_x", m_f_x, 320.0);
      nh.param("/f_y", m_f_y, 320.0);
      nh.param("/c_x", m_c_x, 320.5);
      nh.param("/c_y", m_c_y, 240.5);

      nh.param<std::string>("/bbox_out_topic", m_bbox_out_topic, "/bboxes");
      nh.param<std::string>("/depth_topic", m_depth_topic, "/d455/depth/image_raw");
      nh.param<std::string>("/depth_base", m_depth_base, "camera_link");
      nh.param<std::string>("/body_base", m_body_base, "body_base");
      nh.param<std::string>("/fixed_frame", m_fixed_frame, "map");


      ///// sub pub
      m_depth_sub = nh.subscribe<sensor_msgs::Image>(m_depth_topic, 10, &ieee_uav_class::depth_callback, this);
      m_tf_sub = nh.subscribe<tf2_msgs::TFMessage>("/tf", 10, &ieee_uav_class::tf_callback, this);
      m_bbox_sub = nh.subscribe<yolo_ros_simple::bboxes>(m_bbox_out_topic, 10, &ieee_uav_class::bbox_callback, this);

      m_detected_target_pcl_pub = nh.advertise<sensor_msgs::PointCloud2>("/detected_target_pcl", 10);

      m_octo_update_and_publisher = nh.createTimer(ros::Duration(1/m_octomap_hz), &ieee_uav_class::octomap_Timer, this); // every hz
      m_local_planner_thread = nh.createTimer(ros::Duration(1/20.0), &ieee_uav_class::local_planner_Timer, this); // every 1/20 second.

      m_best_branch_pub = nh.advertise<nav_msgs::Path>("/best_path", 10);

      ROS_WARN("Class heritated, starting node...");
    }
};








/////////////////////////////////////////// can be separated into .cpp source file from here

void ieee_uav_class::new_path_callback(const std_msgs::Bool::ConstPtr& msg){
  m_new_path=true;
}

void ieee_uav_class::depth_callback(const sensor_msgs::Image::ConstPtr& msg){
  sensor_msgs::Image depth=*msg;

  try {
    pcl::PointXYZ p3d, p3d_empty;
    // tic(); 
    m_depth_cvt_pcl.clear();
    m_depth_cvt_pcl_empty.clear();
    if (depth.encoding=="32FC1"){
      m_depth_ptr = cv_bridge::toCvCopy(depth, "32FC1"); // == sensor_msgs::image_encodings::TYPE_32FC1
      m_scale_factor=1.0;
      for (int i=0; i<m_depth_ptr->image.rows; i++){
        for (int j=0; j<m_depth_ptr->image.cols; j++){
          float temp_depth = m_depth_ptr->image.at<float>(i,j); //float!!! double makes error here!!! because encoding is "32FC", float
          if (std::isnan(temp_depth)){
            p3d_empty.z = m_pcl_max_range * cos(abs(m_depth_ptr->image.cols/2.0 - j)/(m_depth_ptr->image.cols/2.0)*m_hfov/2.0);
            p3d_empty.x = ( j - m_c_x ) * p3d_empty.z / m_f_x;
            p3d_empty.y = ( i - m_c_y ) * p3d_empty.z / m_f_y;
            m_depth_cvt_pcl_empty.push_back(p3d_empty);
          }
          else if (temp_depth >= 0.1 and temp_depth <= m_pcl_max_range){
            p3d.z = temp_depth; 
            p3d.x = ( j - m_c_x ) * p3d.z / m_f_x;
            p3d.y = ( i - m_c_y ) * p3d.z / m_f_y;
            m_depth_cvt_pcl.push_back(p3d);
          }
        }
      }
    }
    else if (depth.encoding=="16UC1"){ // uint16_t (stdint.h) or ushort or unsigned_short
      m_depth_ptr = cv_bridge::toCvCopy(depth, "16UC1"); // == sensor_msgs::image_encodings::TYPE_16UC1
      m_scale_factor=1000.0;
      for (int i=0; i<m_depth_ptr->image.rows; i++){
        for (int j=0; j<m_depth_ptr->image.cols; j++){
          float temp_depth = m_depth_ptr->image.at<ushort>(i,j); //ushort!!! other makes error here!!! because encoding is "16UC"
          if (std::isnan(temp_depth)){
            p3d_empty.z = m_pcl_max_range * cos(abs(m_depth_ptr->image.cols/2.0 - j)/(m_depth_ptr->image.cols/2.0)*m_hfov/2.0);
            p3d_empty.x = ( j - m_c_x ) * p3d_empty.z / m_f_x;
            p3d_empty.y = ( i - m_c_y ) * p3d_empty.z / m_f_y;
            m_depth_cvt_pcl_empty.push_back(p3d_empty);
          }
          else if (temp_depth/m_scale_factor >= 0.1 and temp_depth/m_scale_factor <= m_pcl_max_range){
            p3d.z = (temp_depth/m_scale_factor); 
            p3d.x = ( j - m_c_x ) * p3d.z / m_f_x;
            p3d.y = ( i - m_c_y ) * p3d.z / m_f_y;
            m_depth_cvt_pcl.push_back(p3d);
          }
        }
      }
    }
    m_depth_check=true;
    // toc();
  }
  catch (cv_bridge::Exception& e) {
    ROS_ERROR("Error to cvt depth img");
    return;
  }
}


void ieee_uav_class::bbox_callback(const yolo_ros_simple::bboxes::ConstPtr& msg){
  if (msg->bboxes.size() < 1)
    return;

  yolo_ros_simple::bbox in_bbox = msg->bboxes[0];

  if (m_depth_check){
    geometry_msgs::Point p3d_center;
    pcl::PointXYZ p3d;
    int pcl_size=0;
    for (int i=in_bbox.y; i < in_bbox.y + in_bbox.height; i++){
      for (int j=in_bbox.x; j < in_bbox.x + in_bbox.width; j++){
        float temp_depth = 0.0;
        if (m_scale_factor==1.0){
          temp_depth = m_depth_ptr->image.at<float>(i,j); //float!!! double makes error here!!! because encoding is "32FC", float
        }
        else if (m_scale_factor==1000.0){
          temp_depth = m_depth_ptr->image.at<ushort>(i,j); //ushort!!! other makes error here!!! because encoding is "16UC"
        }
        if (std::isnan(temp_depth) or temp_depth==0.0){
          continue;
        }
        else if (temp_depth/m_scale_factor >= 0.1 and temp_depth/m_scale_factor <= m_pcl_max_range){
          p3d.z = (temp_depth/m_scale_factor); 
          p3d.x = ( j - m_c_x ) * p3d.z / m_f_x;
          p3d.y = ( i - m_c_y ) * p3d.z / m_f_y;

          p3d_center.x += p3d.x;
          p3d_center.y += p3d.y;
          p3d_center.z += p3d.z;
          pcl_size++;
        }
      }
    }
    if (pcl_size>0){
      p3d_center.x /= (float)pcl_size;
      p3d_center.y /= (float)pcl_size;
      p3d_center.z /= (float)pcl_size;

      m_detected_center = p3d_center;

      pcl::PointCloud<pcl::PointXYZ>::Ptr detected_center_pub(new pcl::PointCloud<pcl::PointXYZ>());
      detected_center_pub->push_back( pcl::PointXYZ(p3d_center.x, p3d_center.y-0.5, p3d_center.z) );
      m_detected_target_pcl_pub.publish(cloud2msg(*detected_center_pub, m_depth_base));
      m_bbox_check=true;
    }
  }
}

void ieee_uav_class::pcl_callback(const sensor_msgs::PointCloud2::ConstPtr& msg){
  m_pcl_in.clear();
  m_pcl_in = cloudmsg2cloud(*msg);
  m_pcl_check=true;
}

void ieee_uav_class::tf_callback(const tf2_msgs::TFMessage::ConstPtr& msg){
  for (int l=0; l < msg->transforms.size(); l++){
    if (msg->transforms[l].header.frame_id==m_fixed_frame && msg->transforms[l].child_frame_id==m_body_base){
      ///// for tf between map and body
      tf::Quaternion q(msg->transforms[l].transform.rotation.x, msg->transforms[l].transform.rotation.y, msg->transforms[l].transform.rotation.z, msg->transforms[l].transform.rotation.w);
      tf::Matrix3x3 m(q);
      m_map_t_body(0,0) = m[0][0];
      m_map_t_body(0,1) = m[0][1];
      m_map_t_body(0,2) = m[0][2];
      m_map_t_body(1,0) = m[1][0];
      m_map_t_body(1,1) = m[1][1];
      m_map_t_body(1,2) = m[1][2];
      m_map_t_body(2,0) = m[2][0];
      m_map_t_body(2,1) = m[2][1];
      m_map_t_body(2,2) = m[2][2];

      m_map_t_body(0,3) = msg->transforms[l].transform.translation.x;
      m_map_t_body(1,3) = msg->transforms[l].transform.translation.y;
      m_map_t_body(2,3) = msg->transforms[l].transform.translation.z;
      m_map_t_body(3,3) = 1.0;
      m.getRPY(m_curr_roll, m_curr_pitch, m_curr_yaw);

      m_cvt_quat_x = q.getX();
      m_cvt_quat_y = q.getY();
      m_cvt_quat_z = q.getZ();
      m_cvt_quat_w = q.getW();
    }
    if (msg->transforms[l].child_frame_id==m_pcl_base && !m_body_t_lidar_check){
      tf::Quaternion q2(msg->transforms[l].transform.rotation.x, msg->transforms[l].transform.rotation.y, msg->transforms[l].transform.rotation.z, msg->transforms[l].transform.rotation.w);
      tf::Matrix3x3 m2(q2);
      m_body_t_lidar(0,0) = m2[0][0];
      m_body_t_lidar(0,1) = m2[0][1];
      m_body_t_lidar(0,2) = m2[0][2];
      m_body_t_lidar(1,0) = m2[1][0];
      m_body_t_lidar(1,1) = m2[1][1];
      m_body_t_lidar(1,2) = m2[1][2];
      m_body_t_lidar(2,0) = m2[2][0];
      m_body_t_lidar(2,1) = m2[2][1];
      m_body_t_lidar(2,2) = m2[2][2];

      m_body_t_lidar(0,3) = msg->transforms[l].transform.translation.x;
      m_body_t_lidar(1,3) = msg->transforms[l].transform.translation.y;
      m_body_t_lidar(2,3) = msg->transforms[l].transform.translation.z;
      m_body_t_lidar(3,3) = 1.0;

      m_body_t_lidar_check = true; // fixed!!!
    }
    if (msg->transforms[l].child_frame_id==m_depth_base && !m_body_t_cam_check){
      tf::Quaternion q2(msg->transforms[l].transform.rotation.x, msg->transforms[l].transform.rotation.y, msg->transforms[l].transform.rotation.z, msg->transforms[l].transform.rotation.w);
      tf::Matrix3x3 m2(q2);
      m_body_t_cam(0,0) = m2[0][0];
      m_body_t_cam(0,1) = m2[0][1];
      m_body_t_cam(0,2) = m2[0][2];
      m_body_t_cam(1,0) = m2[1][0];
      m_body_t_cam(1,1) = m2[1][1];
      m_body_t_cam(1,2) = m2[1][2];
      m_body_t_cam(2,0) = m2[2][0];
      m_body_t_cam(2,1) = m2[2][1];
      m_body_t_cam(2,2) = m2[2][2];

      m_body_t_cam(0,3) = msg->transforms[l].transform.translation.x;
      m_body_t_cam(1,3) = msg->transforms[l].transform.translation.y;
      m_body_t_cam(2,3) = msg->transforms[l].transform.translation.z;
      m_body_t_cam(3,3) = 1.0;

      m_body_t_cam_check = true; // fixed!!!
    }
  }
  
  m_map_t_lidar = m_map_t_body * m_body_t_lidar ;
  m_map_t_cam = m_map_t_body * m_body_t_cam;
  if (m_body_t_cam_check && m_body_t_lidar_check)
    m_tf_check=true;
}


void ieee_uav_class::octomap_Timer(const ros::TimerEvent& event){
  if (m_pcl_check && m_depth_check && m_tf_check){

    pcl::PointCloud<pcl::PointXYZ> depth_cvt_pcl_map, depth_cvt_pcl_map_empty, pcl_in_map;

    pcl::transformPointCloud(m_depth_cvt_pcl, depth_cvt_pcl_map, m_map_t_cam); //essential for agg_pcl and octomap(based on world naturally!)
    pcl::transformPointCloud(m_depth_cvt_pcl_empty, depth_cvt_pcl_map_empty, m_map_t_cam); //essential for agg_pcl and octomap(based on world naturally!)
    pcl::transformPointCloud(m_pcl_in, pcl_in_map, m_map_t_lidar); //essential for octomap(based on world naturally!)

    /////// octomap update with RGB-D empty cell within sensor range
    octomap::KeySet free_cells_keyset;
    for (pcl::PointCloud<pcl::PointXYZ>::const_iterator it = depth_cvt_pcl_map_empty.begin(); it!=depth_cvt_pcl_map_empty.end(); ++it){
      octomap::KeyRay ray;
      m_octree->computeRayKeys(octomap::point3d(m_map_t_cam(0,3),m_map_t_cam(1,3),m_map_t_cam(2,3)), octomap::point3d(it->x, it->y, it->z), ray);
      free_cells_keyset.insert(ray.begin(), ray.end());
    } 
    for (octomap::KeySet::iterator it = free_cells_keyset.begin(); it!=free_cells_keyset.end(); it++){
      m_octree->updateNode(*it, false); // set as free
    }

    /////// octomap updating with RGB-D occupied cell
    octomap::Pointcloud temp_pcl2; 
    for (pcl::PointCloud<pcl::PointXYZ>::const_iterator it = depth_cvt_pcl_map.begin(); it!=depth_cvt_pcl_map.end(); ++it){
      temp_pcl2.push_back(it->x, it->y, it->z);
    } 
    m_octree->insertPointCloud(temp_pcl2, octomap::point3d(m_map_t_cam(0,3),m_map_t_cam(1,3),m_map_t_cam(2,3))); 

    /////// octomap updating with RGB-D occupied cell
    octomap::Pointcloud temp_pcl; 
    for (pcl::PointCloud<pcl::PointXYZ>::const_iterator it = pcl_in_map.begin(); it!=pcl_in_map.end(); ++it){
      temp_pcl.push_back(it->x, it->y, it->z);
    } 
    m_octree->insertPointCloud(temp_pcl, octomap::point3d(m_map_t_lidar(0,3),m_map_t_lidar(1,3),m_map_t_lidar(2,3))); 
    // octomap is originally based on world(0,0,0)!!! ray should be from sensing robot, or occupancy will be calculated from the point to sensing, wrong occupancy
    // does not remove using rays, but just stack

    pcl::PointCloud<pcl::PointXYZ>::Ptr octo_pcl_pub(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::PointCloud<pcl::PointXYZ>::Ptr octo_pcl_empty_pub(new pcl::PointCloud<pcl::PointXYZ>());
    for (octomap::OcTree::iterator it=m_octree->begin(); it!=m_octree->end(); ++it){
      if(m_octree->isNodeOccupied(*it))
      {
        // if (it.getCoordinate().z()<2.2)
        octo_pcl_pub->push_back(pcl::PointXYZ(it.getCoordinate().x(), it.getCoordinate().y(), it.getCoordinate().z()));
      }
      else
      {
        octo_pcl_empty_pub->push_back(pcl::PointXYZ(it.getCoordinate().x(), it.getCoordinate().y(), it.getCoordinate().z()));
        // free_oc++;
      }
    }
    m_octo_check=true;
    m_octomap_pub.publish(cloud2msg(*octo_pcl_pub, m_fixed_frame));
    m_octomap_empty_pub.publish(cloud2msg(*octo_pcl_empty_pub, m_fixed_frame));
  }
}


void ieee_uav_class::local_planner_Timer(const ros::TimerEvent& event){
  if (m_octo_check && g_local_init){
    // tic();
    pcl::PointCloud<pcl::PointXYZ>::Ptr contact_pcl_pub(new pcl::PointCloud<pcl::PointXYZ>());
    int id = 1;
    int sid = 1000;
    bool big_branch_ok = false;
    g_loc_score = MatrixXd::Zero(g_row, g_col); 
    for (int i=0; i < g_row; i++){
      for (int j=0; j < g_col; j++){
        visualization_msgs::Marker path;
        path.ns = "loc";
        path.header.frame_id = m_fixed_frame;
        path.type=4;
        path.scale.x = 0.01; //width
        path.id = id;
        path.color.r = 1.0;
        path.color.b = 1.0;
        path.color.a = 1.0;
        path.lifetime = ros::Duration(1/20.0);
        path.pose.position.x = m_map_t_body(0,3);
        path.pose.position.y = m_map_t_body(1,3);
        path.pose.position.z = m_map_t_body(2,3);
        path.pose.orientation.x = m_cvt_quat_x;
        path.pose.orientation.y = m_cvt_quat_y;
        path.pose.orientation.z = m_cvt_quat_z;
        path.pose.orientation.w = m_cvt_quat_w;
        // path.header.stamp = ros::Time::now();

        float t = 0.4*g_T;
        geometry_msgs::Point p;
        p.x = g_cx(i,j,0)*pow(t,5) + g_cx(i,j,1)*pow(t,4) + g_cx(i,j,2)*pow(t,3) + g_cx(i,j,3)*pow(t,2) + g_cx(i,j,4)*t + g_cx(i,j,5);
        p.y = g_cy(i,j,0)*pow(t,5) + g_cy(i,j,1)*pow(t,4) + g_cy(i,j,2)*pow(t,3) + g_cy(i,j,3)*pow(t,2) + g_cy(i,j,4)*t + g_cy(i,j,5);
        p.z = g_cz(i,0)*pow(t,5) + g_cz(i,1)*pow(t,4) + g_cz(i,2)*pow(t,3) + g_cz(i,3)*pow(t,2) + g_cz(i,4)*t + g_cz(i,5);
        geometry_msgs::Point tf_p = tf_point(p, m_map_t_body);
        Vector3d p1(tf_p.x, tf_p.y, tf_p.z);

        t = g_T;
        p.x = g_cx(i,j,0)*pow(t,5) + g_cx(i,j,1)*pow(t,4) + g_cx(i,j,2)*pow(t,3) + g_cx(i,j,3)*pow(t,2) + g_cx(i,j,4)*t + g_cx(i,j,5);
        p.y = g_cy(i,j,0)*pow(t,5) + g_cy(i,j,1)*pow(t,4) + g_cy(i,j,2)*pow(t,3) + g_cy(i,j,3)*pow(t,2) + g_cy(i,j,4)*t + g_cy(i,j,5);
        p.z = g_cz(i,0)*pow(t,5) + g_cz(i,1)*pow(t,4) + g_cz(i,2)*pow(t,3) + g_cz(i,3)*pow(t,2) + g_cz(i,4)*t + g_cz(i,5);
        tf_p = tf_point(p, m_map_t_body);
        Vector3d p2(tf_p.x, tf_p.y, tf_p.z);
        big_branch_ok = !(collisionLine_and_traversable(m_octree, p1, p2, m_collision_r));

        if(big_branch_ok){
          for (double t=0.1*g_T; t<= g_T; t+=0.1*g_T){
            geometry_msgs::Point p;
            p.x = g_cx(i,j,0)*pow(t,5) + g_cx(i,j,1)*pow(t,4) + g_cx(i,j,2)*pow(t,3) + g_cx(i,j,3)*pow(t,2) + g_cx(i,j,4)*t + g_cx(i,j,5);
            p.y = g_cy(i,j,0)*pow(t,5) + g_cy(i,j,1)*pow(t,4) + g_cy(i,j,2)*pow(t,3) + g_cy(i,j,3)*pow(t,2) + g_cy(i,j,4)*t + g_cy(i,j,5);
            p.z = g_cz(i,0)*pow(t,5) + g_cz(i,1)*pow(t,4) + g_cz(i,2)*pow(t,3) + g_cz(i,3)*pow(t,2) + g_cz(i,4)*t + g_cz(i,5);

            geometry_msgs::Point tf_p = tf_point(p, m_map_t_body);
            path.points.push_back(p);
          }

          if (id%2==0) m_local_branch_pub.publish(path); else m_local_branch_pub2.publish(path);
          id++;

          bool s_branch_ok = false;
          for (int b=0; b < g_branch; b++){
            visualization_msgs::Marker spath;
            spath.ns = "s_loc";
            spath.header.frame_id = m_fixed_frame;
            spath.type=4;
            spath.scale.x = 0.01; //width
            spath.id = sid;
            spath.color.g = 1.0;
            spath.color.b = 1.0;
            spath.color.a = 1.0;
            spath.lifetime = ros::Duration(1/20.0);
            spath.pose.position.x = m_map_t_body(0,3);
            spath.pose.position.y = m_map_t_body(1,3);
            spath.pose.position.z = m_map_t_body(2,3);
            spath.pose.orientation.x = m_cvt_quat_x;
            spath.pose.orientation.y = m_cvt_quat_y;
            spath.pose.orientation.z = m_cvt_quat_z;
            spath.pose.orientation.w = m_cvt_quat_w;
            // spath.header.stamp = ros::Time::now();

            for (double t=0.0; t<= g_T; t+=0.1*g_T){
              geometry_msgs::Point p;
              p.x = g_scx(i,j,b,0)*pow(t,5) + g_scx(i,j,b,1)*pow(t,4) + g_scx(i,j,b,2)*pow(t,3) + g_scx(i,j,b,3)*pow(t,2) + g_scx(i,j,b,4)*t + g_scx(i,j,b,5);
              p.y = g_scy(i,j,b,0)*pow(t,5) + g_scy(i,j,b,1)*pow(t,4) + g_scy(i,j,b,2)*pow(t,3) + g_scy(i,j,b,3)*pow(t,2) + g_scy(i,j,b,4)*t + g_scy(i,j,b,5);
              p.z = g_scz(i,0)*pow(t,5) + g_scz(i,1)*pow(t,4) + g_scz(i,2)*pow(t,3) + g_scz(i,3)*pow(t,2) + g_scz(i,4)*t + g_scz(i,5);

              geometry_msgs::Point tf_p = tf_point(p, m_map_t_body);

              auto *ptr = m_octree->search(tf_p.x,tf_p.y,tf_p.z,m_search_depth); //x,y,z,depth=0(full)
              if (ptr){ // if not NULL : searched before
                if (m_octree->isNodeOccupied(ptr)) // occupied
                {
                  s_branch_ok=false;
                  break;
                }
                else{ // not occupied -> free space
                  octomap::point3d end_point;
                  if (m_octree->castRay(octomap::point3d(tf_p.x, tf_p.y, tf_p.z), octomap::point3d(0, 0, -1), end_point, false, 0.8)){
                    g_loc_score(i,j)+=m_free_score_local;
                    spath.points.push_back(p);
                    s_branch_ok=true;
                    contact_pcl_pub->push_back(pcl::PointXYZ(end_point.x(), end_point.y(), end_point.z()+m_octomap_resolution));
                  }
                  else{
                    s_branch_ok=false;
                    break;
                  }
                }
              }
              else{ // not searched yet -> unknown space
                octomap::point3d end_point;
                if (m_octree->castRay(octomap::point3d(tf_p.x, tf_p.y, tf_p.z), octomap::point3d(0, 0, -1), end_point, false, 0.8)){
                  g_loc_score(i,j)+=m_unknown_score_local;
                  spath.points.push_back(p);
                  s_branch_ok=true;
                  contact_pcl_pub->push_back(pcl::PointXYZ(end_point.x(), end_point.y(), end_point.z()+m_octomap_resolution));
                }
                else{
                  s_branch_ok=false;
                  break;
                }
              }
            }
            if (s_branch_ok){
              if (sid<1000+g_row*g_col)
                m_local_branch_pub10.publish(spath);
              else if(sid<1000+g_row*g_col*2)
                m_local_branch_pub11.publish(spath);
              else if(sid<1000+g_row*g_col*3)
                m_local_branch_pub12.publish(spath);
              else if(sid<1000+g_row*g_col*4)
                m_local_branch_pub13.publish(spath);
              else if(sid<1000+g_row*g_col*5)
                m_local_branch_pub14.publish(spath);
              else if(sid<1000+g_row*g_col*6)
                m_local_branch_pub15.publish(spath);
              else
                m_local_branch_pub16.publish(spath);
              sid++;
            }
          }
        }
      }
    }
    if (!contact_pcl_pub->empty())
      m_contact_points_pub.publish(cloud2msg(*contact_pcl_pub, m_fixed_frame));
    // toc("peacock");

    if (m_bbox_check){
      ///// if close enough, then stay still
      MatrixXf center_after_tf(4,1);
      center_after_tf << m_detected_center.x, m_detected_center.y, m_detected_center.z, 1.0;
      center_after_tf = m_map_t_cam * center_after_tf;
      if (euclidean_dist(center_after_tf(0), center_after_tf(1), center_after_tf(2), m_map_t_body(0,3), m_map_t_body(1,3), m_map_t_body(2,3)) < 1.5){
        nav_msgs::Path path;
        path.header.frame_id = m_fixed_frame;
        for (int i = 0; i < 10; ++i){
          geometry_msgs::PoseStamped p;
          p.pose.position.x = m_map_t_body(0,3);
          p.pose.position.y = m_map_t_body(1,3);
          p.pose.position.z = m_map_t_body(2,3);

          p.pose.orientation.x = m_cvt_quat_x;
          p.pose.orientation.y = m_cvt_quat_y;
          p.pose.orientation.z = m_cvt_quat_z;
          p.pose.orientation.w = m_cvt_quat_w;
          path.poses.push_back(p);
        }
        m_best_branch_pub.publish(path);
        m_new_path=false;
        return;
      }

      if(m_new_path){
        g_distance_score=MatrixXd::Zero(g_row,g_col);
        for (int i = 0; i < g_row; i++){
          for (int j = 0; j < g_col; j++){
            geometry_msgs::Point ppp;
            ppp.x = g_x_2(i,j); ppp.y = g_y_2(i,j); ppp.z = g_z_2(i,0);
            geometry_msgs::Point tf_p = tf_point(ppp, m_map_t_body);
            g_distance_score(i,j) = euclidean_dist(center_after_tf(0), center_after_tf(1), center_after_tf(2), tf_p.x, tf_p.y, tf_p.z);
          }
        }
        g_distance_score=g_distance_score.cwiseInverse()*10.0;
        best_score_path(g_loc_score, g_distance_score);

        if (m_score_debug) 
          {cout << g_max_score << " " << g_best_row << " " << g_best_col << endl << g_loc_score << endl << g_distance_score <<endl << endl;}

        nav_msgs::Path path;
        path.header.frame_id = m_fixed_frame;
        geometry_msgs::PoseStamped p;
        for (double t=0.0; t<= g_T; t+=0.1*g_T){
          p.pose.position.x = g_cx(g_best_row,g_best_col,0)*pow(t,5) + g_cx(g_best_row,g_best_col,1)*pow(t,4) + g_cx(g_best_row,g_best_col,2)*pow(t,3) + g_cx(g_best_row,g_best_col,3)*pow(t,2) + g_cx(g_best_row,g_best_col,4)*t + g_cx(g_best_row,g_best_col,5);
          p.pose.position.y = g_cy(g_best_row,g_best_col,0)*pow(t,5) + g_cy(g_best_row,g_best_col,1)*pow(t,4) + g_cy(g_best_row,g_best_col,2)*pow(t,3) + g_cy(g_best_row,g_best_col,3)*pow(t,2) + g_cy(g_best_row,g_best_col,4)*t + g_cy(g_best_row,g_best_col,5);
          p.pose.position.z = g_cz(g_best_row,0)*pow(t,5) + g_cz(g_best_row,1)*pow(t,4) + g_cz(g_best_row,2)*pow(t,3) + g_cz(g_best_row,3)*pow(t,2) + g_cz(g_best_row,4)*t + g_cz(g_best_row,5);
          tf::Quaternion q;
          q.setRPY(0, 0, g_yaw(g_best_row,g_best_col)*t/g_T);
          p.pose.orientation.x = q.getX();
          p.pose.orientation.y = q.getY();
          p.pose.orientation.z = q.getZ();
          p.pose.orientation.w = q.getW();

          geometry_msgs::PoseStamped tf_p = tf_pose(p, m_map_t_body);
          path.poses.push_back(tf_p);
        }
        m_best_branch_pub.publish(path);
        m_new_path=false;
      }
    }
  }
}





#endif