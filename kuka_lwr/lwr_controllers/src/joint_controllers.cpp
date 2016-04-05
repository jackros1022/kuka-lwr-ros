#include <angles/angles.h>
#include <pluginlib/class_list_macros.h>
#include <algorithm>
#include <kdl/tree.hpp>
#include <kdl/chainfksolvervel_recursive.hpp>
#include <kdl_parser/kdl_parser.hpp>
#include <urdf/model.h>
#include <tf/transform_broadcaster.h>
#include "lwr_controllers/joint_controllers.h"
#include "utils/safety.h"

int thrott_time = 5;

namespace lwr_controllers {

JointControllers::JointControllers() {}

JointControllers::~JointControllers() {}

bool JointControllers::init(hardware_interface::PositionJointInterface *robot, ros::NodeHandle &n)
{

    KinematicChainControllerBase<hardware_interface::PositionJointInterface>::init(robot, n);

    if ( !n.getParam("max_qdot",max_dqot) ){
        ROS_WARN_STREAM("maximum allowed velocity not set in parameter server, default value of 1 rad/s set");
        max_dqot=1.0;
    }

    K_.resize(kdl_chain_.getNrOfJoints());
    D_.resize(kdl_chain_.getNrOfJoints());

    K_cmd.resize(kdl_chain_.getNrOfJoints());
    D_cmd.resize(kdl_chain_.getNrOfJoints());
    pos_cmd_.resize(kdl_chain_.getNrOfJoints());
    tau_cmd_.resize(kdl_chain_.getNrOfJoints());


    D_.data.setZero();
    K_.data.setZero();
    tau_cmd_.data.setZero();


    ROS_INFO("Loading resource for stiffness values");
    // Get joint handles for all of the joints in the chain
    for(std::size_t i = 0; i < joint_handles_.size(); i++)
    {
        joint_handles_stiffness.push_back(robot->getHandle(kdl_chain_.segments[i].getJoint().getName()  + "_stiffness"));
        joint_handles_damping.push_back(robot->getHandle(kdl_chain_.segments[i].getJoint().getName()    + "_damping"));
        joint_handles_torque.push_back(robot->getHandle(kdl_chain_.segments[i].getJoint().getName()     + "_torque"));
    }

    qdot_msg.data.resize(kdl_chain_.getNrOfJoints());




    /// one_task_inverse_kinematics stuff
    jnt_to_jac_solver_.reset(new KDL::ChainJntToJacSolver(kdl_chain_));
    fk_pos_solver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));
    ik_vel_solver_.reset(new KDL::ChainIkSolverVel_pinv(kdl_chain_));
    ik_pos_solver_.reset(new KDL::ChainIkSolverPos_NR_JL(kdl_chain_,joint_limits_.min,joint_limits_.max,*fk_pos_solver_,*ik_vel_solver_));

    ff_fb_controller.reset(new controllers::FF_FB_cartesian(nh_,change_ctrl_mode,ik_vel_solver_));
    cartesian_controller.reset(new controllers::Open_loop_cartesian(nh_,change_ctrl_mode,ik_vel_solver_));
    joint_position_controller.reset(new controllers::Joint_position(nh_,change_ctrl_mode));
    gravity_compensation_controller.reset(new controllers::Gravity_compensation(nh_,change_ctrl_mode));

    change_ctrl_mode.add(ff_fb_controller.get());
    change_ctrl_mode.add(cartesian_controller.get());
    change_ctrl_mode.add(joint_position_controller.get());
    change_ctrl_mode.add(gravity_compensation_controller.get());

    J_.resize(kdl_chain_.getNrOfJoints());

    // get joint positions
    for(std::size_t i=0; i < joint_handles_.size(); i++)
    {
        joint_msr_.q(i)    = joint_handles_[i].getPosition();
        joint_msr_.qdot(i) = joint_handles_[i].getVelocity();
        joint_des_.q(i)    = joint_msr_.q(i);
        joint_des_.qdot(i) = 0;
        pos_cmd_(i)      = joint_des_.q(i);

    }

    sub_stiff_             = nh_.subscribe("stiffness",        1, &JointControllers::setStiffness,         this);
    sub_damp_              = nh_.subscribe("damping",          1, &JointControllers::setDamping,           this);
    sub_command_string_    = nh_.subscribe("command_string",   1, &JointControllers::command_string,       this);

    pub_qdot_              = n.advertise<std_msgs::Float64MultiArray>("qdot",10);
    pub_F_                 = n.advertise<std_msgs::Float64MultiArray>("F_ee",10);
    pub_tau_               = n.advertise<std_msgs::Float64MultiArray>("tau_cmd",10);

    nd1 = ros::NodeHandle("D_param");
    nd2 = ros::NodeHandle("K_param");
    nd3 = ros::NodeHandle("D_all_param");
    nd4 = ros::NodeHandle("K_all_param");

    dynamic_server_D_param.reset(new        dynamic_reconfigure::Server< lwr_controllers::damping_paramConfig   >(nd1));
    dynamic_server_K_param.reset(new        dynamic_reconfigure::Server< lwr_controllers::stiffness_paramConfig >(nd2));

    dynamic_server_D_all_param.reset(new    dynamic_reconfigure::Server< lwr_controllers::damping_param_allConfig   >(nd3));
    dynamic_server_K_all_param.reset(new    dynamic_reconfigure::Server< lwr_controllers::stiffness_param_allConfig >(nd4));


    dynamic_server_D_param->setCallback(     boost::bind(&JointControllers::damping_callback,      this, _1, _2));
    dynamic_server_K_param->setCallback(     boost::bind(&JointControllers::stiffness_callback,    this, _1, _2));

    dynamic_server_D_all_param->setCallback( boost::bind(&JointControllers::damping_all_callback,  this, _1, _2));
    dynamic_server_K_all_param->setCallback( boost::bind(&JointControllers::stiffness_all_callback,this, _1, _2));



    publish_rate_          = 100;



    F_msg.data.resize(6);
    tau_msg.data.resize(7);

    std::cout<< "finished init" << std::endl;
    return true;
}

void JointControllers::starting(const ros::Time& time)
{
    ROS_INFO("JointControllers::starting");
    // get joint positions
    for(size_t i=0; i<joint_handles_.size(); i++) {
        joint_msr_.q(i)    = joint_handles_[i].getPosition();
        joint_msr_.qdot(i) = joint_handles_[i].getVelocity();
        joint_des_.q(i)    = joint_msr_.q(i);
        pos_cmd_(i)        = joint_des_.q(i);
        tau_cmd_(i)        = 0;
        K_cmd(i)           = 0;
        D_cmd(i)           = 0.7;

        joint_handles_[i].setCommand(pos_cmd_(i));
        joint_handles_torque[i].setCommand(tau_cmd_(i));
        joint_handles_stiffness[i].setCommand(K_cmd(i));
        joint_handles_damping[i].setCommand(D_cmd(i));
    }
    std::cout<< "finished starting" << std::endl;
}

void JointControllers::update(const ros::Time& time, const ros::Duration& period)
{
    // get measured joint positions and velocity
    for(size_t i=0; i<joint_handles_.size(); i++) {
        joint_msr_.q(i)           = joint_handles_[i].getPosition();
        joint_msr_.qdot(i)        = joint_handles_[i].getVelocity();
        qdot_msg.data[i]          = joint_msr_.qdot(i);
    }
    pub_qdot_.publish(qdot_msg);
    jnt_to_jac_solver_->JntToJac(joint_msr_.q,J_);
    fk_pos_solver_->JntToCart(joint_msr_.q, x_);


    if(change_ctrl_mode.is_switching())
    {
        change_ctrl_mode.switching();
        ctrl_mode = CTRL_MODE::GRAV_COMP;
    }else{

        ctrl_mode           = change_ctrl_mode.get_ctrl_mode();

        switch(ctrl_mode)
        {
        case CTRL_MODE::CART_VELOCITIY:
        {
            ROS_INFO_STREAM_THROTTLE(thrott_time,"ctrl_mode ===> CART_VELOCITIY");
            cartesian_controller->cart_vel_update(tau_cmd_,joint_des_,joint_msr_,K_,D_,period);
            robot_ctrl_mode = ROBOT_CTRL_MODE::TORQUE_IMP;
            break;
        }
        case CTRL_MODE::FF_FB_CARTESIAN:
        {
            ROS_INFO_STREAM_THROTTLE(thrott_time,"ctrl_mode ===> FF_FB_FORCE");
            ff_fb_controller->cart_ff_fb_update(tau_cmd_,joint_des_,joint_msr_,K_,D_,period);
            robot_ctrl_mode = ROBOT_CTRL_MODE::TORQUE_IMP;
            break;
        }
        case CTRL_MODE::JOINT_POSITION:
        {
            ROS_INFO_STREAM_THROTTLE(thrott_time,"ctrl_mode ===> JOINT_POSITION");
            joint_position_controller->update(joint_des_,joint_msr_,period);
            robot_ctrl_mode = ROBOT_CTRL_MODE::POSITION_IMP;
            break;
        }
        case CTRL_MODE::GRAV_COMP:
        {
            ROS_INFO_STREAM_THROTTLE(thrott_time,"ctrl_mode ===> GRAV_COMP");
            gravity_compensation_controller->update(tau_cmd_,pos_cmd_,K_cmd,D_cmd,joint_des_,joint_msr_);
            robot_ctrl_mode = ROBOT_CTRL_MODE::TORQUE_IMP;
            break;
        }
        default:    // same as grav-comp
        {
            ROS_INFO_STREAM_THROTTLE(thrott_time,"ctrl_mode ===> NONE");
            for(std::size_t i = 0; i < joint_handles_.size();i++){
                tau_cmd_(i)         = 0;
                joint_des_.q(i)     = joint_msr_.q(i);
                joint_des_.qdot(i)  = 0;
                robot_ctrl_mode     = ROBOT_CTRL_MODE::TORQUE_IMP;
            }
            break;
        }
        }
    }

    if(robot_ctrl_mode == ROBOT_CTRL_MODE::TORQUE_IMP)
    {
        ROS_INFO_STREAM_THROTTLE(thrott_time,"   TORQUE_IMP    ");
        for(size_t i=0; i<joint_handles_.size(); i++) {
            K_cmd(i)         = 0;
            D_cmd(i)         = 0;
            pos_cmd_(i)      = joint_msr_.q(i);
        }
    }else if(ctrl_mode != CTRL_MODE::GRAV_COMP){
        ROS_INFO_STREAM_THROTTLE(thrott_time,"   POSITION_IMP   ");
        for(size_t i=0; i<joint_handles_.size(); i++) {
            K_cmd(i)         = K_(i);
            D_cmd(i)         = D_(i);
            tau_cmd_(i)      = 0;
            pos_cmd_(i)      = joint_des_.q(i);
        }
    }

    ROS_INFO_STREAM_THROTTLE(thrott_time,"--------------");
    ROS_INFO_STREAM_THROTTLE(thrott_time,"K_cmd:    " << K_cmd(0) << " " << K_cmd(1) << " " << K_cmd(2) << " " << K_cmd(3) << " " << K_cmd(4) << " " << K_cmd(5) << " " << K_cmd(6));
    ROS_INFO_STREAM_THROTTLE(thrott_time,"D_cmd:    " << D_cmd(0) << " " << D_cmd(1) << " " << D_cmd(2) << " " << D_cmd(3) << " " << D_cmd(4) << " " << D_cmd(5) << " " << D_cmd(6));
    ROS_INFO_STREAM_THROTTLE(thrott_time,"tau_cmd_: " << tau_cmd_(0) << " " <<  tau_cmd_(1) << " " <<  tau_cmd_(2) << " " << tau_cmd_(3) << " " <<  tau_cmd_(4) << " " <<  tau_cmd_(5) << " " << tau_cmd_(6));
    ROS_INFO_STREAM_THROTTLE(thrott_time,"pos_cmd_: " << pos_cmd_(0) << " " <<  pos_cmd_(1) << " " <<  pos_cmd_(2) << " " << pos_cmd_(3) << " " <<  pos_cmd_(4) << " " <<  pos_cmd_(5) << " " << pos_cmd_(6));
    ROS_INFO_STREAM_THROTTLE(thrott_time,"--------------");

    /// Safety check if measured joint velocity is above specified threashold set torque and command to zero

    if(lwr::safety::is_too_fast(joint_msr_.qdot,max_dqot)){
        for(size_t i=0; i<joint_handles_.size(); i++) {
            K_cmd(i)               = 0;
            D_cmd(i)               = 0.01;
            tau_cmd_(i)            = 0;
            pos_cmd_(i)            = joint_msr_.q(i);
            joint_des_.q(i)        = joint_msr_.q(i);
            joint_des_.qdot(i)     = 0;
        }
    }

    for(size_t i=0; i<joint_handles_.size(); i++) {
        tau_msg.data[i] =tau_cmd_(i);
        joint_handles_stiffness[i].setCommand(K_cmd(i));
        joint_handles_damping[i].setCommand(D_cmd(i));
        joint_handles_torque[i].setCommand(tau_cmd_(i));
        joint_handles_[i].setCommand(pos_cmd_(i));

    }
    // pub_tau_.publish(tau_msg);
}



void JointControllers::command_string(const std_msgs::String::ConstPtr& msg){
    if(msg->data == "velocity_open")
    {
        // cart_type = VELOCITY_OPEN_LOOP;
        ROS_INFO("cartesian velocity [OPEN LOOP]");
    }else if(msg->data == "velocity_ps")
    {
        //cart_type = VELOCITY_PASSIVE_DS;
        ROS_INFO("cartesian velocity [PASSIVE DS]");
    }else{
        ROS_WARN_STREAM("No such string command [" << msg->data << "]  [JointControllers::command_string]");
    }
}


void JointControllers::setStiffness(const std_msgs::Float64MultiArray::ConstPtr &msg){
    if (msg->data.size() == joint_handles_.size()){
        for (std::size_t i = 0; i < joint_handles_.size(); ++i){
            K_(i)       = msg->data[i];
        }
    }
    else
    {
        ROS_INFO("Stiffness Num of Joint handles = %lu", joint_handles_.size());
    }
}

void JointControllers::setDamping(const std_msgs::Float64MultiArray::ConstPtr &msg){
    if (msg->data.size() == joint_handles_.size()){
        for (std::size_t i = 0; i < joint_handles_.size(); ++i){
            D_(i)    = msg->data[i];
        }
    }else
    {
        ROS_INFO("Damping Num of Joint handles = %lu", joint_handles_.size());
    }
}

void JointControllers::damping_callback(damping_paramConfig &config, uint32_t level){
    D_(0) = config.damp_0_joint;
    D_(1) = config.damp_1_joint;
    D_(2) = config.damp_2_joint;
    D_(3) = config.damp_3_joint;
    D_(4) = config.damp_4_joint;
    D_(5) = config.damp_5_joint;
    D_(6) = config.damp_6_joint;
}

void JointControllers::stiffness_callback(lwr_controllers::stiffness_paramConfig& config, uint32_t level){
    K_(0) = config.K_0_joint;
    K_(1) = config.K_1_joint;
    K_(2) = config.K_2_joint;
    K_(3) = config.K_3_joint;
    K_(4) = config.K_4_joint;
    K_(5) = config.K_5_joint;
    K_(6) = config.K_6_joint;
}

void JointControllers::damping_all_callback(lwr_controllers::damping_param_allConfig& config,uint32_t level){
    for(std::size_t i = 0; i < joint_handles_.size();i++){
        D_(i)    = config.D;
    }

}

void JointControllers::stiffness_all_callback(lwr_controllers::stiffness_param_allConfig& config, uint32_t level){
    for(std::size_t i = 0; i < joint_handles_.size();i++){
        K_(i)    = config.K;
    }
}


}                                                           // namespace

PLUGINLIB_EXPORT_CLASS( lwr_controllers::JointControllers, controller_interface::ControllerBase)
