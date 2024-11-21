#include <target_detection/sim_detect.h>

// Sim_detect 构造函数，初始化节点句柄和参数
Sim_detect::Sim_detect(ros::NodeHandle& nh){
    // 读取ROS参数服务器中的参数，若参数不存在，则使用默认值
    nh.param("grid_map/resolution", resolution, 0.1);  // 地图分辨率，默认为0.1
    nh.param("grid_map/map_size_x", map_size_3d_[0], 100.0);  // 地图尺寸x方向，默认为100.0
    nh.param("grid_map/map_size_y", map_size_3d_[1], 100.0);  // 地图尺寸y方向，默认为100.0
    nh.param("grid_map/map_size_z", map_size_3d_[2], 3.0);    // 地图尺寸z方向，默认为3.0

    // 设置地图原点位置，默认位于地图中心的底部
    origin_[0] = -map_size_3d_(0) / 2;
    origin_[1] = -map_size_3d_(1) / 2;
    origin_[2] = -0.1;

    // 计算每个维度的格子数量
    dim_(0) = map_size_3d_(0) / resolution;
    dim_(1) = map_size_3d_(1) / resolution;
    dim_(2) = map_size_3d_(2) / resolution;

    // 计算地图缓存的大小，并分配空间
    buffer_size = dim_(0) * dim_(1) * dim_(2);
    map_buffer.resize(buffer_size);

    // 订阅话题，用于接收全局地图、车辆状态和无人机位姿
    map_sub = nh.subscribe("global_map", 10, &Sim_detect::GlobalMapBuild, this);  // 接收全局地图
    target_sub = nh.subscribe("car_state_detect", 1, &Sim_detect::car_state_cb, this);  // 接收车辆状态
    drone_sub = nh.subscribe("drone_odom", 1, &Sim_detect::drone_odom_cb, this);  // 接收无人机位姿

    // 发布话题，用于发布目标检测结果
    detection_pub = nh.advertise<nav_msgs::Odometry>("target", 1, true);  // 发布目标信息
}

// 处理全局地图，生成网格地图
void Sim_detect::GlobalMapBuild(const sensor_msgs::PointCloud2 & pointcloud_map){
    if (has_map) return;  // 如果地图已经生成，直接返回

    pcl::PointCloud<pcl::PointXYZ> cloud;
    pcl::fromROSMsg(pointcloud_map, cloud);  // 将 PointCloud2 消息转换为 PCL 格式

    if ((int)cloud.points.size() == 0) return;  // 如果点云为空，直接返回

    pcl::PointXYZ pt;
    ROS_INFO("cloud points size=%d\n", (int)cloud.points.size());  // 打印点云大小
    for (int idx = 0; idx < (int)cloud.points.size(); idx++) {
        pt = cloud.points[idx];
        setObs(Eigen::Vector3d(pt.x, pt.y, pt.z));  // 将点云中每个点加入到地图中
    }
    has_map = true;  // 标记地图已经生成
}

// 设置障碍物
void Sim_detect::setObs(Eigen::Vector3d pt){
    int expand_size = 0;  // 障碍物扩展尺寸（当前未使用扩展）
    Eigen::Vector3i index = FloatToint(pt);  // 将浮点坐标转换为离散网格坐标
    if (isOutside(index)) return;  // 如果点在地图外，跳过处理

    // 在扩展范围内设置障碍物
    for (int i = -expand_size; i <= expand_size; i++)
        for (int j = -expand_size; j <= expand_size; j++)
            for (int k = -expand_size; k <= expand_size; k++) {
                Eigen::Vector3i temp_index;
                temp_index(0) = index(0) + i;
                temp_index(1) = index(1) + j;
                temp_index(2) = index(2) + k;
                if (isOutside(temp_index)) continue;  // 如果扩展后的坐标在地图外，跳过
                map_buffer[getIndex(temp_index)] = 1;  // 设置地图缓存对应位置为占用
            }
}

// 判断点是否在地图范围外
bool Sim_detect::isOutside(Eigen::Vector3i idx){
    for (int i = 0; i < 3; i++)
        if (idx(i) < 0 || idx(i) >= dim_(i))  // 检查每个维度是否越界
            return true;
    return false;
}

// 将3D网格索引转换为一维索引
int Sim_detect::getIndex(Eigen::Vector3i idx){
    return idx(0) + dim_(0) * idx(1) + dim_(0) * dim_(1) * idx(2);
}

// 判断点是否被占用
bool Sim_detect::isOccupied(Eigen::Vector3i idx){
    if (isOutside(idx)) {
        return true;  // 如果在地图外，视为占用
    }
    return map_buffer[getIndex(idx)];  // 返回对应缓存位置的值
}

// 将离散网格坐标转换为连续世界坐标
Eigen::Vector3d Sim_detect::intToFloat(const Eigen::Vector3i idx){
    return (idx.template cast<double>() + Eigen::Vector3d::Constant(0.5)) * resolution + origin_;
}

// 将连续世界坐标转换为离散网格坐标
Eigen::Vector3i Sim_detect::FloatToint(const Eigen::Vector3d pt){
    Eigen::Vector3i pn;
    for (int i = 0; i < 3; i++)
        pn(i) = std::round((pt(i) - origin_(i)) / resolution - 0.5);
    return pn;
}

// 判断两点之间是否有障碍物
bool Sim_detect::is_block(Eigen::Vector3d pos1, Eigen::Vector3d pos2){
    double t;
    for (t = 0.0; t <= 1; t += 0.01) {  // 按0.01的步长对两点之间插值
        Eigen::Vector3d tmp_pos = t * pos1 + (1 - t) * pos2;  // 线性插值计算中间点
        if (isOccupied(FloatToint(tmp_pos))) {  // 如果中间点被占用，返回true
            return true;
        }
    }
    return false;  // 如果没有障碍物，返回false
}

// 处理车辆状态的回调函数
void Sim_detect::car_state_cb(const nav_msgs::Odometry& car_state){
    if (!has_map) return;  // 如果地图未生成，跳过处理
    Eigen::Vector3d car_pos;
    car_pos << car_state.pose.pose.position.x, car_state.pose.pose.position.y, car_state.pose.pose.position.z;

    // 检测车辆和无人机之间是否有障碍物
    if (!is_block(car_pos, drone_pos)) {
        nav_msgs::Odometry state_pub = car_state;
        state_pub.header.stamp = ros::Time::now();  // 更新时间戳
        detection_pub.publish(state_pub);  // 发布车辆状态
    }
}

// 处理无人机位姿的回调函数
void Sim_detect::drone_odom_cb(const nav_msgs::Odometry& odom){
    drone_pos << odom.pose.pose.position.x, odom.pose.pose.position.y, odom.pose.pose.position.z;  // 更新无人机位置
}
