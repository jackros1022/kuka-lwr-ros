
#ifndef LWR_CONTROLLERS__JOINT_INPEDANCE_CONTROLLER_H
#define LWR_CONTROLLERS__JOINT_INPEDANCE_CONTROLLER_H

#include "KinematicChainControllerBase.h"

#include <ros/ros.h>

#include <dynamic_reconfigure/server.h>

#include <lwr_controllers/damping_paramConfig.h>
#include <lwr_controllers/passive_ds_paramConfig.h>
#include <lwr_controllers/stiffness_paramConfig.h>
#include <lwr_controllers/stiffness_param_allConfig.h>
#include <lwr_controllers/damping_param_allConfig.h>

#include <std_msgs/Float64MultiArray.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Int32.h>
#include <std_msgs/String.h>

#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>

#include <tf/transform_broadcaster.h>


#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/chainiksolvervel_pinv.hpp>
#include <kdl/chainiksolverpos_nr_jl.hpp>


#include <boost/scoped_ptr.hpp>

#include "controllers/gravity_compensation.h"
#include "controllers/joint_position.h"
#include "controllers/open_loop_cartesian.h"
#include "controllers/ff_fb_cartesian.h"
#include "controllers/change_ctrl_mode.h"

#include "utils/definitions.h"


/*
	tau_cmd_ = K_*(q_des_ - q_msr_) + D_*dotq_msr_ + G(q_msr_)

*/

namespace lwr_controllers
{

    class JointControllers: public controller_interface::KinematicChainControllerBase<hardware_interface::PositionJointInterface>
	{

	public:

        JointControllers();

        ~JointControllers();

        bool init(hardware_interface::PositionJointInterface *robot, ros::NodeHandle &n);

		void starting(const ros::Time& time);

		void update(const ros::Time& time, const ros::Duration& period);

    private:

        void command_set_cart_type(const std_msgs::Int32& msg);
        void setStiffness(const std_msgs::Float64MultiArray::ConstPtr &msg);
        void setDamping(const std_msgs::Float64MultiArray::ConstPtr &msg);
        void command_string(const std_msgs::String::ConstPtr& msg);

    private:

        void damping_callback(lwr_controllers::damping_paramConfig& config,uint32_t level);

        void stiffness_callback(lwr_controllers::stiffness_paramConfig& config, uint32_t level);

        void damping_all_callback(lwr_controllers::damping_param_allConfig& config,uint32_t level);

        void stiffness_all_callback(lwr_controllers::stiffness_param_allConfig& config, uint32_t level);

	private:

        controllers::Change_ctrl_mode                          change_ctrl_mode;
        boost::scoped_ptr<controllers::FF_FB_cartesian>            ff_fb_controller;
        boost::scoped_ptr<controllers::Open_loop_cartesian>    cartesian_controller;
        boost::scoped_ptr<controllers::Joint_position>         joint_position_controller;
        boost::scoped_ptr<controllers::Gravity_compensation>   gravity_compensation_controller;

		ros::Subscriber sub_gains_;
		ros::Subscriber sub_posture_;

        ros::Subscriber sub_stiff_;
        ros::Subscriber sub_damp_;
        ros::Subscriber sub_command_string_;

        ros::Publisher pub_qdot_,pub_F_,pub_tau_;
        std_msgs::Float64MultiArray qdot_msg, F_msg, tau_msg;

        KDL::JntArray       tau_cmd_;
        KDL::JntArray       pos_cmd_;
        KDL::JntArray       K_, D_,K_cmd,D_cmd;

        double              max_dqot; // maximum allowed joint velocity (radians).

        std::size_t         num_ctrl_joints;

        KDL::Frame          x_;		//current pose
        KDL::Jacobian       J_;	//Jacobian

        ros::Time           last_publish_time_;
        double              publish_rate_;


        struct quaternion_
        {
            KDL::Vector v;
            double a;
        } quat_curr_, quat_des_;

        KDL::Vector v_temp_;

        bool cmd_flag_;

        boost::scoped_ptr<KDL::ChainDynParam>               id_solver_gravity_;
        boost::scoped_ptr<KDL::ChainJntToJacSolver>         jnt_to_jac_solver_;

        boost::shared_ptr<KDL::ChainFkSolverPos_recursive>  fk_pos_solver_;
        boost::shared_ptr<KDL::ChainIkSolverVel_pinv>       ik_vel_solver_;
        boost::shared_ptr<KDL::ChainIkSolverPos_NR_JL>      ik_pos_solver_;


        boost::scoped_ptr< dynamic_reconfigure::Server< lwr_controllers::damping_paramConfig> >         dynamic_server_D_param;
        boost::scoped_ptr< dynamic_reconfigure::Server< lwr_controllers::stiffness_paramConfig> >       dynamic_server_K_param;
        boost::scoped_ptr< dynamic_reconfigure::Server< lwr_controllers::damping_param_allConfig> >     dynamic_server_D_all_param;
        boost::scoped_ptr< dynamic_reconfigure::Server< lwr_controllers::stiffness_param_allConfig> >   dynamic_server_K_all_param;


        ros::NodeHandle nd1, nd2, nd3,nd4, nd6;

        lwr_controllers::CTRL_MODE       ctrl_mode;
        lwr_controllers::ROBOT_CTRL_MODE robot_ctrl_mode;

        /// Extra handles
        std::vector<hardware_interface::PositionJointInterface::ResourceHandleType> joint_handles_damping;
        std::vector<hardware_interface::PositionJointInterface::ResourceHandleType> joint_handles_stiffness;
        std::vector<hardware_interface::PositionJointInterface::ResourceHandleType> joint_handles_torque;

	};

} // namespace

#endif
