/*
 * File: proxy.cpp
 * Author: Minjun Xu (mjxu96@gmail.com)
 * File Created: Saturday, 6th July 2019 10:11:52 pm
 */

#include "proxy/proxy.h"

using namespace mellocolate;
// For readable seconds
using namespace std::chrono_literals;
using namespace std::string_literals;

// For websocket
using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;

std::pair<double, double> AfterRotate(double x, double y, double yaw) {
  return {std::cos(yaw)*x - std::sin(yaw)*y, std::sin(yaw)*x + std::cos(yaw)*y};
}

Proxy::Proxy(boost::shared_ptr<carla::client::Client> client_ptr, boost::asio::ip::tcp::socket socket) 
  : client_ptr_(std::move(client_ptr)), 
    ws_ptr_(boost::make_shared<websocket::stream<tcp::socket>>(std::move(socket))) {
  world_ptr_ = boost::make_shared<carla::client::World>(client_ptr_->GetWorld());
}

void Proxy::Run() { 
  Init(); 
  while (true) {
    auto world_snapshots = world_ptr_->WaitForTick(2s);
    Update(GetUpdateData(world_snapshots));
  }
}

void Proxy::Init() {
  try {
    ws_ptr_->accept();
    LOG_INFO("Frontend connected");
    boost::beast::multi_buffer buffer;
    boost::beast::ostream(buffer) << GetMetaData();
    ws_ptr_->write(buffer.data());
  } catch(boost::system::system_error const& se) {
    if(se.code() != websocket::error::closed) {
      throw se;
    } else {
      LOG_INFO("Frontend connection closed");
    }
  }
  
}

void Proxy::Update(const std::string& data_str) {

  boost::beast::multi_buffer buffer;
  
  boost::beast::ostream(buffer) << data_str;//GetUpdateData();

  try {
    ws_ptr_->write(buffer.data());
  } catch(boost::system::system_error const& se) {
    if(se.code() != websocket::error::closed) {
      throw se;
    } else {
      LOG_INFO("Frontend connection closed");
    }
  }
}

std::string Proxy::GetMetaData() {
  std::string map_geojson = utils::XodrGeojsonConverter::GetGeoJsonFromCarlaMap(world_ptr_->GetMap());
  XVIZMetaDataBuilder xviz_metadata_builder;
  xviz_metadata_builder
    .SetMap(map_geojson)
    .AddStream(metadata::Stream("/vehicle_pose")
      .AddCategory("pose"))
    .AddStream(metadata::Stream("/object/shape")
      .AddCategory("primitive")
      .AddCoordinate("IDENTITY")
      .AddStreamStyle(metadata::StreamStyle()
        .AddExtruded(true)
        .AddFillColor("#fb0")
        .AddHeight(1.5))
      .AddType("polygon"))
    .AddStream(metadata::Stream("/lidar/points")
      .AddCategory("primitive")
      .AddCoordinate("IDENTITY")
      .AddType("points")
      .AddStreamStyle(metadata::StreamStyle()
        .AddPointCloudMode("distance_to_vehicle")
        .AddRadiusPixels(2.0)));
  return xviz_metadata_builder.GetMetaData();
}

std::string Proxy::GetUpdateData(const carla::client::WorldSnapshot& world_snapshots) {
  std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
  double now_time = now.time_since_epoch().count() / 1e9;

  XVIZBuilder xviz_builder;
  xviz_builder
    .AddTimestamp(now_time)
    .AddPose(XVIZPoseBuilder("/vehicle_pose")
      .AddMapOrigin(point_3d_t(0, 0, 0))
      .AddOrientation(point_3d_t(0, 0, 0))
      .AddPosition(point_3d_t(0, 0, 0))
      .AddTimestamp(now_time));
  XVIZPrimitiveBuider xviz_primitive_builder("/object/shape");

  std::unordered_map<uint32_t, boost::shared_ptr<carla::client::Actor>> tmp_actors;
  std::unordered_map<uint32_t, boost::shared_ptr<carla::client::Sensor>> tmp_sensors;

  for (const auto& world_snapshot : world_snapshots) {
    uint32_t id = world_snapshot.id;
    auto actor_it = actors_.find(id);
    boost::shared_ptr<carla::client::Actor> actor_ptr = nullptr;
    if (actor_it == actors_.end()) {
      actor_ptr = world_ptr_->GetActor(id);
    } else {
      actor_ptr = actor_it->second;
    }
    if (actor_ptr == nullptr) {
      LOG_WARNING("Actor pointer is null, actor id: %u", id);
      continue;
    }
    tmp_actors.insert({id, actor_ptr});
    if (actor_ptr->GetTypeId().substr(0, 6) == "sensor") {
      auto sensor_ptr = boost::static_pointer_cast<carla::client::Sensor>(actor_ptr);
      auto sensor_it = sensors_.find(id);
      if (sensor_it == sensors_.end()) {
        LOG_INFO("Listen sensor: %u", id);
        sensor_ptr->Listen(
          [this, id] (carla::SharedPtr<carla::sensor::SensorData> data) {
            if (data == nullptr) {
              return;
            }
            std::lock_guard<std::mutex> lock_guard(this->sensor_data_queue_lock_);
            lidar_data_queues_[id] = this->GetPointCloud(*(boost::static_pointer_cast<carla::sensor::data::LidarMeasurement>(data)));
        });
      }
      tmp_sensors.insert({id, sensor_ptr});
    }
  }

  actors_ = std::move(tmp_actors);

  std::vector<uint32_t> to_delete_sensor_ids;
  for (const auto& sensor_pair : sensors_) {
    auto id = sensor_pair.first;
    if (tmp_sensors.find(id) == tmp_sensors.end()) {
      to_delete_sensor_ids.push_back(id);
    }
  }
  for (const auto& id : to_delete_sensor_ids) {
    LOG_INFO("Stop listening sensor: %u", id);
    sensors_[id]->Stop();
    std::lock_guard<std::mutex> lock_guard(this->sensor_data_queue_lock_);
    lidar_data_queues_.erase(id);
  }
  sensors_ = std::move(tmp_sensors);

  for (const auto& actor_pair : actors_) {
    auto actor_ptr = actor_pair.second;

    if (actor_ptr->GetTypeId().substr(0, 2) == "ve") {
      AddVehicle(xviz_primitive_builder, actor_ptr);
    }

  }


  sensor_data_queue_lock_.lock();
  XVIZPrimitiveBuider point_cloud_builder("/lidar/points");
  for (const auto& point_cloud_pair : lidar_data_queues_) {
    point_cloud_builder.AddPoints(XVIZPrimitivePointBuilder(point_cloud_pair.second));
  }
  sensor_data_queue_lock_.unlock();

  xviz_builder
    .AddPrimitive(xviz_primitive_builder)
    .AddPrimitive(point_cloud_builder);
  return xviz_builder.GetData();
}

void Proxy::AddVehicle(XVIZPrimitiveBuider& xviz_primitive_builder, boost::shared_ptr<carla::client::Actor> actor_ptr) {
  auto bounding_box = (boost::static_pointer_cast<carla::client::Vehicle>(actor_ptr))->GetBoundingBox();
  double x_off = bounding_box.extent.x;
  double y_off = bounding_box.extent.y;
  double yaw = actor_ptr->GetTransform().rotation.yaw / 180.0 * M_PI;
  std::vector<std::pair<double, double>> offset = {AfterRotate(-x_off, -y_off, yaw), AfterRotate(-x_off, y_off, yaw),
            AfterRotate(x_off, y_off, yaw), AfterRotate(x_off, -y_off, yaw)};
  double x = actor_ptr->GetLocation().x;
  double y = actor_ptr->GetLocation().y;
  double z = actor_ptr->GetLocation().z;
  std::vector<point_3d_t> vertices;
  for (int j = 0; j < offset.size(); j++) {
    vertices.emplace_back(x + offset[j].first, -(y + offset[j].second), z);
  }
  xviz_primitive_builder
      .AddPolygon(XVIZPrimitivePolygonBuilder(vertices)
        .AddId(actor_ptr->GetTypeId() + std::to_string(actor_ptr->GetId())));
}
// std::string Proxy::GetUpdateData() {
//   std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
//   double now_time = now.time_since_epoch().count() / 1e9;

//   XVIZBuilder xviz_builder;
//   xviz_builder
//     .AddTimestamp(now_time)
//     .AddPose(XVIZPoseBuilder("/vehicle_pose")
//       .AddMapOrigin(point_3d_t(0, 0, 0))
//       .AddOrientation(point_3d_t(0, 0, 0))
//       .AddPosition(point_3d_t(0, 0, 0))
//       .AddTimestamp(now_time));
//   XVIZPrimitiveBuider xviz_primitive_builder("/object/shape");

//   for (const auto& sensor_pair : sensors_) {
//     sensor_pair.second->Stop();
//   }
//   sensors_.clear();

//   auto actor_list = world_ptr_->GetActors();
//   for (const auto& actor : *actor_list) {
//     if (actor->GetTypeId().substr(0, 6) == "sensor") {
//       uint32_t id = actor->GetId();
//       sensors_.insert({id, boost::static_pointer_cast<carla::client::Sensor>(actor)});
//       (boost::static_pointer_cast<carla::client::Sensor>(actor))->Listen(
//         [this, id, &actor] (carla::SharedPtr<carla::sensor::SensorData> data) {
//           if (data == nullptr) {
//             return;
//           }
//           std::lock_guard<std::mutex> lock_guard(this->sensor_data_queue_lock_);
//           lidar_data_queues_[id] = this->GetPointCloud(*(boost::static_pointer_cast<carla::sensor::data::LidarMeasurement>(data)));
//         }
//       );
//     }

//     if (actor->GetTypeId().substr(0, 2) != "ve") {
//       continue;
//     }
//     auto bounding_box = (boost::static_pointer_cast<carla::client::Vehicle>(actor))->GetBoundingBox();
//     double x_off = bounding_box.extent.x;
//     double y_off = bounding_box.extent.y;
//     double yaw = actor->GetTransform().rotation.yaw / 180.0 * M_PI;
//     std::vector<std::pair<double, double>> offset = {AfterRotate(-x_off, -y_off, yaw), AfterRotate(-x_off, y_off, yaw),
//               AfterRotate(x_off, y_off, yaw), AfterRotate(x_off, -y_off, yaw)};
//     double x = actor->GetLocation().x;
//     double y = actor->GetLocation().y;
//     double z = actor->GetLocation().z;
//     std::vector<point_3d_t> vertices;
//     for (int j = 0; j < offset.size(); j++) {
//       vertices.emplace_back(x + offset[j].first, -(y + offset[j].second), z);
//     }
//     xviz_primitive_builder
//         .AddPolygon(XVIZPrimitivePolygonBuilder(vertices)
//           .AddId(actor->GetTypeId() + std::to_string(actor->GetId())));
//   }

//   xviz_builder
//     .AddPrimitive(xviz_primitive_builder);

//   // Add point cloud primitive
//   XVIZPrimitiveBuider point_cloud_builder("/lidar/points");
//   sensor_data_queue_lock_.lock();
//   std::vector<uint32_t> to_be_delete_id;
//   for (const auto& point_cloud_pair : lidar_data_queues_) {
//     if (sensors_.find(point_cloud_pair.first) != sensors_.end()) {
//       point_cloud_builder.AddPoints(XVIZPrimitivePointBuilder(point_cloud_pair.second));
//     } else {
//       to_be_delete_id.push_back(point_cloud_pair.first);
//     }
//   }
//   for (auto id : to_be_delete_id) {
//     lidar_data_queues_.erase(id);
//   }
//   sensor_data_queue_lock_.unlock();
//   xviz_builder.AddPrimitive(point_cloud_builder);

//   return xviz_builder.GetData();
// }


std::vector<point_3d_t> Proxy::GetPointCloud(const carla::sensor::data::LidarMeasurement& lidar_measurement) {
  std::vector<point_3d_t> points;
  double yaw = lidar_measurement.GetSensorTransform().rotation.yaw;
  auto location = lidar_measurement.GetSensorTransform().location;
  for (const auto& point : lidar_measurement) {
    point_3d_t offset = utils::Utils::GetOffsetAfterTransform(point_3d_t(point.x, point.y, point.z), (yaw+90.0)/180.0 * M_PI);
    points.emplace_back(location.x + offset.get<0>(),
      -(location.y + offset.get<1>()),
      location.z + offset.get<2>());
  }
  //std::cout << "[" << lidar_measurement.GetSensorTransform().location.x << ", " << lidar_measurement.GetSensorTransform().location.y << "]" << std::endl;
  //std::cout << "yaw: " << yaw << std::endl;
  return points;
}


// int main() {
//   Proxy proxy;
//   proxy.Run();
//   return 0;
// }