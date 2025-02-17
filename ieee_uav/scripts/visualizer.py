#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Created on Tue May 19 00:28:30 2020

@author: mason
"""

''' import libraries '''
import time
import numpy as np
from math import sqrt, pow

import rospy
from nav_msgs.msg import Path
from geometry_msgs.msg import PoseStamped
from gazebo_msgs.msg import ModelStates
from std_msgs.msg import Float32

import sys
import signal

def signal_handler(signal, frame): # ctrl + c -> exit program
    print('You pressed Ctrl+C!')
    sys.exit(0)
signal.signal(signal.SIGINT, signal_handler)


''' class '''

class path_pub():
    def __init__(self):
        rospy.init_node('gt_path_pubb', anonymous=True)
        self.parent_frame_id = rospy.get_param("/parent_frame_id", 'map')
        self.append_rate = rospy.get_param("/append_rate", 5)
        self.path_time = rospy.get_param("/path_time", 60)
        self.robot_name = "iris"
        self.target_name = "rover_moving"

        self.gt_path_pub = rospy.Publisher("/gt_robot_path", Path, queue_size=2)
        self.gt_path_pub2 = rospy.Publisher("/gt_target_path", Path, queue_size=2)
        self.distance = rospy.Publisher("/distance", Float32, queue_size=2)
        self.gt_poses = rospy.Subscriber("/gazebo/model_states", ModelStates, self.gtcallback)

        self.robot_path = Path()
        self.target_path = Path()
        self.robot_check = 0
        self.target_check = 0
        self.float_data = Float32()

        self.rate = rospy.Rate(self.append_rate)
        # self.f=open("/home/mason/data.csv", 'a')

    def gtcallback(self, msg):
        for i in range(len(msg.name)):
            if msg.name[i]==self.robot_name:
                self.robot_pose = msg.pose[i]
                self.robot_vel = msg.twist[i]
                self.robot_check= 1
            elif msg.name[i]==self.target_name:
                self.target_pose = msg.pose[i]
                self.target_vel = msg.twist[i]
                self.curr_t=rospy.Time.now().to_sec()
                if self.target_check==0:
                    self.target_pose_last = self.target_pose
                    self.prev_t = self.curr_t-0.001
                    self.Vtarget_prev = 0.0
                self.target_check= 1
        if self.robot_check and self.target_check:
            self.float_data.data = sqrt(pow(self.robot_pose.position.x-self.target_pose.position.x, 2) + \
                                        pow(self.robot_pose.position.y-self.target_pose.position.y, 2) + \
                                        pow(self.robot_pose.position.z-self.target_pose.position.z, 2))
            VTarget = sqrt(pow(self.target_pose.position.x-self.target_pose_last.position.x, 2) + \
                                        pow(self.target_pose.position.y-self.target_pose_last.position.y, 2) + \
                                        pow(self.target_pose.position.z-self.target_pose_last.position.z, 2))
            VTarget = VTarget/(self.curr_t-self.prev_t+0.001)
            self.VTarget = self.Vtarget_prev * 0.9 + VTarget * 0.1

            VUAV = sqrt(pow(self.robot_vel.linear.x, 2) + pow(self.robot_vel.linear.y, 2) + pow(self.robot_vel.linear.z, 2))
            self.distance.publish(self.float_data)
            # self.f.write("%.2f, %.2f, %.2f, %.2f\n"%(self.curr_t, self.float_data.data, self.VTarget, VUAV)) #time, dist, Vtarget, Vuav
            self.target_pose_last = self.target_pose
            self.prev_t = self.curr_t
            self.Vtarget_prev = self.VTarget

''' main '''
path_pub_ = path_pub()

if __name__ == '__main__':
    while 1:
        try:
            if path_pub_.robot_check == 1:
                pose = PoseStamped()
                pose.pose = path_pub_.robot_pose
                pose.header.frame_id = path_pub_.parent_frame_id
                pose.header.stamp = rospy.Time.now()
                path_pub_.robot_path.poses.append(pose)
                path_pub_.robot_path.header.frame_id = path_pub_.parent_frame_id 
                path_pub_.robot_path.header.stamp = rospy.Time.now()
                path_pub_.gt_path_pub.publish(path_pub_.robot_path)
                if len(path_pub_.robot_path.poses) > path_pub_.append_rate*path_pub_.path_time:
                    del path_pub_.robot_path.poses[0]
            if path_pub_.target_check == 1:
                pose = PoseStamped()
                pose.pose = path_pub_.target_pose
                pose.header.frame_id = path_pub_.parent_frame_id
                pose.header.stamp = rospy.Time.now()
                path_pub_.target_path.poses.append(pose)
                path_pub_.target_path.header.frame_id = path_pub_.parent_frame_id 
                path_pub_.target_path.header.stamp = rospy.Time.now()
                path_pub_.gt_path_pub2.publish(path_pub_.target_path)
                if len(path_pub_.target_path.poses) > path_pub_.append_rate*path_pub_.path_time:
                    del path_pub_.target_path.poses[0]
            path_pub_.rate.sleep()
        except (rospy.ROSInterruptException, SystemExit, KeyboardInterrupt) :
            sys.exit(0)
        # except:
        #     print("exception")
        #     pass
