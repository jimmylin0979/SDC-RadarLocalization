#include <string>
#include <fstream>
#include <iostream>
#include <cmath>
#include <unordered_map>

#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl/point_cloud.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/common/transforms.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/passthrough.h>
#include <pcl/registration/icp.h>
#include <pcl_ros/transforms.h>

#include <Eigen/Dense>

#include <tf/tf.h>
#include <tf/transform_broadcaster.h>
#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <nav_msgs/Path.h>

using namespace std;
class Localizer
{
private:
    ros::NodeHandle _nh;

    ros::Subscriber radar_pc_sub;
    ros::Subscriber map_sub;
    ros::Subscriber gps_sub;

    ros::Publisher radar_pc_pub;
    ros::Publisher radar_pose_pub;
    ros::Publisher path_pub;

    pcl::PointCloud<pcl::PointXYZI>::Ptr map_pc;
    nav_msgs::Path path;
    tf::TransformBroadcaster br;

    std::string save_path;
    std::ofstream file;

    float pose_x;
    float pose_y;
    float pose_yaw;
    float gps_x;
    float gps_y;
    float gps_yaw;

    float exp_a = 0.98;
    float pre_pose_x;
    float pre_pose_y;
    float pre_pose_yaw;

    ros::Time pre_time;
    float pre_pose_vx = 0.0;
    float pre_pose_vy = 0.0;
    float pre_pose_vyaw = 0.0;

    int seq = 0;
    int max_iter = 100;
    float epsilon1 = 1e-5;
    float epsilon2 = 1e-5;
    float correspond = 3;

    Eigen::Matrix4f init_guess;

    bool map_ready = false;
    bool gps_ready = false;
    bool initialized = false;

public:
    Localizer(ros::NodeHandle nh) : map_pc(new pcl::PointCloud<pcl::PointXYZI>)
    {
        map_ready = false;
        gps_ready = false;

        _nh = nh;
        _nh.param<string>("/save_path", save_path, "/Default/path");

        init_guess.setIdentity();
        file.open(save_path);
        file << "id,x,y,yaw\n";

        radar_pc_sub = _nh.subscribe("/radar_pc", 400, &Localizer::radar_pc_callback, this);
        map_sub = _nh.subscribe("/map_pc", 1, &Localizer::map_callback, this);
        gps_sub = _nh.subscribe("/gps", 1, &Localizer::gps_callback, this);

        radar_pc_pub = _nh.advertise<sensor_msgs::PointCloud2>("/tranformed_radar_pc", 1);
        radar_pose_pub = _nh.advertise<geometry_msgs::PoseStamped>("/tranformed_radar_pose", 1);
        path_pub = _nh.advertise<nav_msgs::Path>("/localization_path", 1);
    }

    ~Localizer()
    {
        ROS_WARN("Exit Localization");
        file.close();
    }

    void gps_callback(const geometry_msgs::PoseStamped::ConstPtr &msg)
    {
        ROS_WARN("Got GPS data");
        gps_x = msg->pose.position.x;
        gps_y = msg->pose.position.y;
        tf::Quaternion q(
            msg->pose.orientation.x,
            msg->pose.orientation.y,
            msg->pose.orientation.z,
            msg->pose.orientation.w);
        tf::Matrix3x3 m(q);
        double r, p, yaw;
        m.getRPY(r, p, yaw);
        gps_yaw = yaw;
        if (!gps_ready)
        {
            pose_x = gps_x;
            pose_y = gps_y;
            pose_yaw = gps_yaw;
            gps_ready = true;
        }
    }

    void map_callback(const sensor_msgs::PointCloud2::ConstPtr &msg)
    {
        ROS_WARN("Got Map Pointcloud");
        pcl::fromROSMsg(*msg, *map_pc);
        map_ready = true;
    }

    void ICP(
            pcl::PointCloud<pcl::PointXYZI>::Ptr source_pc, 
            pcl::PointCloud<pcl::PointXYZI>::Ptr target_pc, 
            pcl::PointCloud<pcl::PointXYZI>::Ptr output_pc,
            float &pose_x, 
            float &pose_y, 
            float &pose_yaw
        ) {
        /*
        * params
        * input
        *           source_pc    the point cloud to begin from
        *           target_pc    the point cloud which we want cloud_in to look like
        * output
        *           pose_x      pose_x
        *           pose_y      pose_y
        *           pose_yaw    pose_yaw
        * returns
        */

        // ICP         
        // Reference: https://pcl.readthedocs.io/projects/tutorials/en/master/iterative_closest_point.html#iterative-closest-point
        // Reference: https://zhuanlan.zhihu.com/p/107218828
        pcl::IterativeClosestPoint<pcl::PointXYZI, pcl::PointXYZI> icp;

        // 
        // pcl::transformPointCloud(*source_pc, *source_pc, init_guess);
        icp.setInputSource(source_pc);
        icp.setInputTarget(target_pc);

        // set ICP parameters if needed
        icp.setMaxCorrespondenceDistance(correspond);  // m
        icp.setMaximumIterations(max_iter);

        // One of the convergence criterion. 
        // If the sum of differences between current and last transformation is smaller than this threshold, 
        //          the registration succeeded and will terminated.
        icp.setTransformationEpsilon(epsilon1);

        // 
        icp.setEuclideanFitnessEpsilon(epsilon2);
        
        // create res to save the resultant cloud after applying the ICP algorithm.
        // pcl::PointCloud<pcl::PointXYZI> res_pc;
        icp.align(*output_pc, init_guess);   // comment when map_pc -> radar_pc
        // icp.align(*output_pc);

        // if the two PointClouds align correctly (meaning they are both the same cloud merely with some kind of rigid transformation applied to one of them)
        //      then icp.hasConverged() = 1 (true). 
        // it then outputs the fitness score of the final transformation and some information about it.
        if (icp.hasConverged()) {
            // obtain the transformation that aligned source_pc to res_pc
            int score = icp.getFitnessScore();
            Eigen::Matrix4f transformation = icp.getFinalTransformation();
            
            cout << "[DEBUG] ICP converge success!" << endl;
            cout << transformation << endl;
            // printf ("Rotation matrix :\n");
            // printf ("R = | %6.3f %6.3f %6.3f | \n", transformation(0, 0), transformation(0, 1), transformation(0, 2));
            // printf ("    | %6.3f %6.3f %6.3f | \n", transformation(1, 0), transformation(1, 1), transformation(1, 2));
            // printf ("    | %6.3f %6.3f %6.3f | \n", transformation(2, 0), transformation(2, 1), transformation(2, 2));
            // printf ("Translation vector :\n");
            // printf ("t = < %6.3f, %6.3f, %6.3f >\n\n", transformation(0, 3), transformation(1, 3), transformation(2, 3));
        
            pose_x = transformation(0, 3);
            pose_y = transformation(1, 3);

            // assuming rotation is represented as quaternion and converting it to yaw
            // Eigen::Matrix3f rotation_matrix = transformation.block<3, 3>(0, 0);
            // Eigen::Vector3f euler_angles = rotation_matrix.eulerAngles(2, 1, 0); // ZYX order 
            // pose_yaw = euler_angles(2);
            pose_yaw = atan2(transformation(1, 0), transformation(0, 0));

        } else {
            // TODO ROS_ERROR
            cout << "[DEBUG] ICP converge failed!" << endl;
            // float alpha = 0.8;
            // pose_x = alpha * gps_x + (1 - alpha) * pose_x;
			// pose_y = alpha * gps_y + (1 - alpha) * pose_y;
			// pose_yaw = alpha * gps_yaw + (1 - alpha) * pose_yaw;
        }
    }

    pcl::PointCloud<pcl::PointXYZI>::Ptr motion_compensation(pcl::PointCloud<pcl::PointXYZI>::Ptr radar_pc) 
    {
        // Calculate translation & rotation distortion terms 
        float delta_t = 0.25;
        int num_azimuths = 400;

        // Group the point into its azumith slot 
        unordered_map<int, pcl::PointCloud<pcl::PointXYZI>::Ptr> pc_mapping;
        for (int i = 0; i < radar_pc->size(); i++) {
            int azimuth_idx = atan2(radar_pc->points[i].y, radar_pc->points[i].x) / (2 * M_PI / num_azimuths);
            if (pc_mapping.find(azimuth_idx) == pc_mapping.end()) {
                pcl::PointCloud<pcl::PointXYZI>::Ptr pc_ptr(new pcl::PointCloud<pcl::PointXYZI>);
                pc_mapping[azimuth_idx] = pc_ptr;
            }
            pc_mapping[azimuth_idx]->push_back(radar_pc->points[i]);
        }

        pcl::PointCloud<pcl::PointXYZI>::Ptr new_pc(new pcl::PointCloud<pcl::PointXYZI>);
        for (int i = 0; i < num_azimuths; i++) {

            // Calculate the translation & rotation distortion terms
            float time_offset = ((float)i - (float)num_azimuths / 2) * delta_t / 2;
            float distortion_x = time_offset * pre_pose_vx;
            float distortion_y = time_offset * pre_pose_vy;
            float distortion_yaw = time_offset * pre_pose_vyaw;

            //
            cout << "time_offset " << time_offset << "." << endl;
            cout << "distortion_x " << distortion_x << "." << endl;
            cout << "distortion_y " << distortion_y << "." << endl;
            cout << "distortion_yaw " << distortion_yaw << "." << endl;

            // Apply distortion transformation
            Eigen::Translation<float, 2> translation(distortion_x, distortion_y);
            Eigen::Rotation2D<float> rotation(distortion_yaw);
            Eigen::Affine2f distortion_transform = translation * rotation;

            for (int j = 0; j < pc_mapping[i]->size(); j++) {
                Eigen::Vector2f point1(pc_mapping[i]->points[j].x, pc_mapping[i]->points[j].y);
                Eigen::Vector2f transformed_point = distortion_transform * point1;
                
                pcl::PointXYZI point;
                point.x = transformed_point(0);
                point.y = transformed_point(1);;    
                point.z = pc_mapping[i]->points[j].z;
                point.intensity = pc_mapping[i]->points[j].intensity;
                new_pc->push_back(point);
            }
        }

        return new_pc;
    }

    void radar_pc_callback(const sensor_msgs::PointCloud2::ConstPtr &msg)
    {
        ROS_WARN("Got Radar Pointcloud");
        pcl::PointCloud<pcl::PointXYZI>::Ptr radar_pc(new pcl::PointCloud<pcl::PointXYZI>);
        pcl::PointCloud<pcl::PointXYZI>::Ptr output_pc(new pcl::PointCloud<pcl::PointXYZI>);
        pcl::fromROSMsg(*msg, *radar_pc);
        ROS_INFO("point size: %d", radar_pc->width);
        // pcl::PointCloud<pcl::PointXYZI>::Ptr compensated_radar_pc = motion_compensation(radar_pc);
        // ROS_INFO("point size: %d", compensated_radar_pc->width);

        while (!(map_ready && gps_ready))
        {
            ROS_WARN("Wait for map and gps ready");
            ros::Duration(0.1).sleep();
            ros::spinOnce();
        }

        if(!initialized)
        {
            // Initialize initial guess
            set_init_guess(pose_x, pose_y, pose_yaw);
            initialized = true;
            pre_time = ros::Time::now();
        }

        // Record the previous poses
        pre_pose_x = pose_x;
        pre_pose_y = pose_y;
        pre_pose_yaw = pose_yaw;

        // Implenment any scan matching base on initial guess, ICP, NDT, etc.
        // Assign the result to pose_x, pose_y, pose_yaw
        // Use result as next time initial guess
        ICP(radar_pc, map_pc, output_pc, pose_x, pose_y, pose_yaw);    //
        
        // // Update the current poses with exponetial smoothing 
        // pose_x = pose_x * exp_a + pre_pose_x * (1 - exp_a);
        // pose_y = pose_y * exp_a + pre_pose_y * (1 - exp_a);
        // pose_yaw = pose_yaw * exp_a + pre_pose_yaw * (1 - exp_a);

        ros::Time curr_time = ros::Time::now();
        double dt = (curr_time - pre_time).toSec();
        pre_pose_vx = (pose_x - pre_pose_x) / dt;
        pre_pose_vy = (pose_y - pre_pose_y) / dt;
        pre_pose_vyaw = (pose_yaw - pre_pose_yaw) / dt;
        cout << "pre_pose_vx " << pre_pose_vx << "." << endl;
        cout << "pre_pose_vy " << pre_pose_vy << "." << endl;
        cout << "pre_pose_vyaw " << pre_pose_vyaw << "." << endl;

        set_init_guess(pose_x, pose_y, pose_yaw);

        tf_brocaster(pose_x, pose_y, pose_yaw);
        radar_pose_publisher(pose_x, pose_y, pose_yaw);

        sensor_msgs::PointCloud2 radar_pc_msg;
        pcl::toROSMsg(*radar_pc, radar_pc_msg);
        radar_pc_msg.header.stamp = ros::Time::now();
        radar_pc_msg.header.frame_id = "base_link";
        radar_pc_pub.publish(radar_pc_msg);
        ROS_INFO("Publish transformed pc");
        ROS_INFO("[seq %d] x:%.3f, y:%.3f, yaw:%.3f\n", seq, pose_x, pose_y, pose_yaw);

        file << seq << ",";
        file << pose_x << ",";
        file << pose_y << ",";
        file << pose_yaw << "\n";

        seq++;
    }

    void radar_pose_publisher(float x, float y, float yaw)
    {
        geometry_msgs::PoseStamped pose;
        tf2::Quaternion myQuaternion;
        myQuaternion.setRPY(0, 0, yaw);
        myQuaternion.normalize();

        pose.header.stamp = ros::Time::now();
        pose.header.frame_id = "map";

        pose.pose.position.x = x;
        pose.pose.position.y = y;
        pose.pose.position.z = 0;

        pose.pose.orientation.x = myQuaternion.getX();
        pose.pose.orientation.y = myQuaternion.getY();
        pose.pose.orientation.z = myQuaternion.getZ();
        pose.pose.orientation.w = myQuaternion.getW();
        radar_pose_pub.publish(pose);

        path.header.frame_id = "map";
        pose.header.stamp = ros::Time::now();
        path.poses.push_back(pose);
        path_pub.publish(path);
    }

    void tf_brocaster(float x, float y, float yaw)
    {
        ROS_INFO("Update map to baselink");
        tf::Transform transform;
        transform.setOrigin(tf::Vector3(x, y, 0));
        tf::Quaternion q;
        q.setRPY(0, 0, yaw);
        transform.setRotation(q);

        br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "map", "base_link"));
    }

    void set_init_guess(float x, float y, float yaw)
    {
        // Set up the init_guess transformation matrix
        init_guess(0, 0) = cos(yaw);
        init_guess(0, 1) = -sin(yaw);
        init_guess(0, 3) = x;

        init_guess(1, 0) = sin(yaw);
        init_guess(1, 1) = cos(yaw);
        init_guess(1, 3) = y;
    }
};

int main(int argc, char **argv)
{
    ros::init(argc, argv, "localizer");
    ros::NodeHandle nh;
    Localizer Localizer(nh);

    ros::spin();
    return 0;
}