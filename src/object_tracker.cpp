#include "libobjecttracker/object_tracker.h"

// PCL
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/transforms.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/transformation_estimation_2D.h>
// #include <pcl/registration/transformation_estimation_lm.h>

#include <iostream>


typedef pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> ICP;

namespace libobjecttracker {

/////////////////////////////////////////////////////////////

Object::Object(
  size_t markerConfigurationIdx,
  size_t dynamicsConfigurationIdx,
  const Eigen::Affine3f& initialTransformation)
  : m_markerConfigurationIdx(markerConfigurationIdx)
  , m_dynamicsConfigurationIdx(dynamicsConfigurationIdx)
  , m_lastTransformation(initialTransformation)
  , m_lastValidTransform()
  , m_lastTransformationValid(false)
{
}

const Eigen::Affine3f& Object::transformation() const
{
  return m_lastTransformation;
}

bool Object::lastTransformationValid() const
{
  return m_lastTransformationValid;
}

/////////////////////////////////////////////////////////////

ObjectTracker::ObjectTracker(
  const std::vector<DynamicsConfiguration>& dynamicsConfigurations,
  const std::vector<MarkerConfiguration>& markerConfigurations,
  const std::vector<Object>& objects)
  : m_dynamicsConfigurations(dynamicsConfigurations)
  , m_markerConfigurations(markerConfigurations)
  , m_objects(objects)
  , m_initialized(false)
{

}

void ObjectTracker::update(
  pcl::PointCloud<pcl::PointXYZ>::Ptr pointCloud)
{
  runICP(pointCloud);
}

const std::vector<Object>& ObjectTracker::objects() const
{
  return m_objects;
}

bool ObjectTracker::initialize(
  pcl::PointCloud<pcl::PointXYZ>::ConstPtr markers)
{
  ICP icp;
  icp.setMaximumIterations(5);
  icp.setInputTarget(markers);

  // prepare for knn query
  std::vector<int> nearestIdx;
  std::vector<float> nearestSqrDist;
  pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
  kdtree.setInputCloud(markers);

  bool allFitsGood = true;
  // for (Object &object: m_objects)
  for (size_t i = 0; i < m_objects.size(); ++i) {
    Object& object = m_objects[i];
    pcl::PointCloud<pcl::PointXYZ>::Ptr &objMarkers =
      m_markerConfigurations[object.m_markerConfigurationIdx];
    icp.setInputSource(objMarkers);

    // find the points nearest to the object's nominal position
    size_t const objNpts = objMarkers->size();
    nearestIdx.resize(objNpts);
    nearestSqrDist.resize(objNpts);
    // initial pos was loaded into lastTransformation from config file
    Eigen::Vector3f objCenter = object.m_lastTransformation.translation();
    pcl::PointXYZ ctr(objCenter.x(), objCenter.y(), objCenter.z());
    kdtree.nearestKSearch(ctr, objNpts, nearestIdx, nearestSqrDist);

    // compute centroid of nearest points
    pcl::PointXYZ center(0, 0, 0);
    for (int i = 0; i < objNpts; ++i) {
      // really, no operators overloads???? something must be missing
      center.x += (*markers)[nearestIdx[i]].x;
      center.y += (*markers)[nearestIdx[i]].y;
      center.z += (*markers)[nearestIdx[i]].z;
    }
    center.x /= objNpts;
    center.y /= objNpts;
    center.z /= objNpts;

    // try ICP with guesses of many different yaws about knn centroid
    pcl::PointCloud<pcl::PointXYZ> result;
    static int const N_YAW = 20;
    double bestErr = DBL_MAX;
    for (int i = 0; i < N_YAW; ++i) {
      float yaw = i * (2 * M_PI / N_YAW);
      Eigen::Matrix4f tryMatrix = pcl::getTransformation(
        center.x, center.y, center.z, yaw, 0, 0).matrix();
      icp.align(result, tryMatrix);
      double err = icp.getFitnessScore();
      if (err < bestErr) {
        bestErr = err;
        object.m_lastTransformation = icp.getFinalTransformation();
      }
    }

    // check that the best fit was actually good
    static double const INIT_MAX_HAUSDORFF_DIST2 = 0.008 * 0.008; // 8mm
    ICP::PointCloudSource bestCloud;
    transformPointCloud(*objMarkers, bestCloud, object.m_lastTransformation);
    nearestIdx.resize(1);
    nearestSqrDist.resize(1);
    for (size_t i = 0; i < objNpts; ++i) {
      kdtree.nearestKSearch(bestCloud[i], 1, nearestIdx, nearestSqrDist);
      if (nearestSqrDist[0] > INIT_MAX_HAUSDORFF_DIST2) {
        allFitsGood = false;
      }
    }
  }

  return allFitsGood;
}

void ObjectTracker::runICP(
  const pcl::PointCloud<pcl::PointXYZ>::ConstPtr markers)
{
  auto stamp = std::chrono::high_resolution_clock::now();
  m_initialized = m_initialized || initialize(markers);
  if (!m_initialized) {
    std::cout << "Object tracker initialization failed - "
      "check that position is correct, all markers are visible, "
      "and marker configuration matches config file" << std::endl;
  }

  ICP icp;
  // pcl::registration::TransformationEstimationLM<pcl::PointXYZ, pcl::PointXYZ>::Ptr trans(new pcl::registration::TransformationEstimationLM<pcl::PointXYZ, pcl::PointXYZ>);
  // pcl::registration::TransformationEstimation2D<pcl::PointXYZ, pcl::PointXYZ>::Ptr trans(new pcl::registration::TransformationEstimation2D<pcl::PointXYZ, pcl::PointXYZ>);
  // pcl::registration::TransformationEstimation3DYaw<pcl::PointXYZ, pcl::PointXYZ>::Ptr trans(new pcl::registration::TransformationEstimation3DYaw<pcl::PointXYZ, pcl::PointXYZ>);
  // icp.setTransformationEstimation(trans);


  // // Set the maximum number of iterations (criterion 1)
  icp.setMaximumIterations(5);
  // // Set the transformation epsilon (criterion 2)
  // icp.setTransformationEpsilon(1e-8);
  // // Set the euclidean distance difference epsilon (criterion 3)
  // icp.setEuclideanFitnessEpsilon(1);

  icp.setInputTarget(markers);

  // for (auto& object : m_objects) {
  for (size_t i = 0; i < m_objects.size(); ++i) {
    Object& object = m_objects[i];
    object.m_lastTransformationValid = false;

    std::chrono::duration<double> elapsedSeconds = stamp-object.m_lastValidTransform;
    double dt = elapsedSeconds.count();

    // Set the max correspondence distance
    // TODO: take max here?
    const DynamicsConfiguration& dynConf = m_dynamicsConfigurations[object.m_dynamicsConfigurationIdx];
    float maxV = dynConf.maxXVelocity;
    icp.setMaxCorrespondenceDistance(maxV * dt);
    // ROS_INFO("max: %f", maxV * dt);

    // Update input source
    icp.setInputSource(m_markerConfigurations[object.m_markerConfigurationIdx]);

    // Perform the alignment
    pcl::PointCloud<pcl::PointXYZ> result;
    icp.align(result, object.m_lastTransformation.matrix());
    if (!icp.hasConverged()) {
      // ros::Time t = ros::Time::now();
      // ROS_INFO("ICP did not converge %d.%d", t.sec, t.nsec);
      std::cout << "ICP did not converge!" << std::endl;
      continue;
    }

    // Obtain the transformation that aligned cloud_source to cloud_source_registered
    Eigen::Matrix4f transformation = icp.getFinalTransformation();

    Eigen::Affine3f tROTA(transformation);
    float x, y, z, roll, pitch, yaw;
    pcl::getTranslationAndEulerAngles(tROTA, x, y, z, roll, pitch, yaw);

    // Compute changes:
    float last_x, last_y, last_z, last_roll, last_pitch, last_yaw;
    pcl::getTranslationAndEulerAngles(object.m_lastTransformation, last_x, last_y, last_z, last_roll, last_pitch, last_yaw);

    float vx = (x - last_x) / dt;
    float vy = (y - last_y) / dt;
    float vz = (z - last_z) / dt;
    float wroll = (roll - last_roll) / dt;
    float wpitch = (pitch - last_pitch) / dt;
    float wyaw = (yaw - last_yaw) / dt;

    // ROS_INFO("v: %f,%f,%f, w: %f,%f,%f, dt: %f", vx, vy, vz, wroll, wpitch, wyaw, dt);

    if (   fabs(vx) < dynConf.maxXVelocity
        && fabs(vy) < dynConf.maxYVelocity
        && fabs(vz) < dynConf.maxZVelocity
        && fabs(wroll) < dynConf.maxRollRate
        && fabs(wpitch) < dynConf.maxPitchRate
        && fabs(wyaw) < dynConf.maxYawRate
        && fabs(roll) < dynConf.maxRoll
        && fabs(pitch) < dynConf.maxPitch)
    {

      object.m_lastTransformation = tROTA;
      object.m_lastValidTransform = stamp;
      object.m_lastTransformationValid = true;
    } else {
      std::stringstream sstr;
      sstr << "Dynamic check failed" << std::endl;
      if (fabs(vx) >= dynConf.maxXVelocity) {
        sstr << "vx: " << vx << " >= " << dynConf.maxXVelocity << std::endl;
      }
      if (fabs(vy) >= dynConf.maxYVelocity) {
        sstr << "vy: " << vy << " >= " << dynConf.maxYVelocity << std::endl;
      }
      if (fabs(vz) >= dynConf.maxZVelocity) {
        sstr << "vz: " << vz << " >= " << dynConf.maxZVelocity << std::endl;
      }
      if (fabs(wroll) >= dynConf.maxRollRate) {
        sstr << "wroll: " << wroll << " >= " << dynConf.maxRollRate << std::endl;
      }
      if (fabs(wpitch) >= dynConf.maxPitchRate) {
        sstr << "wpitch: " << wpitch << " >= " << dynConf.maxPitchRate << std::endl;
      }
      if (fabs(wyaw) >= dynConf.maxYawRate) {
        sstr << "wyaw: " << wyaw << " >= " << dynConf.maxYawRate << std::endl;
      }
      if (fabs(roll) >= dynConf.maxRoll) {
        sstr << "roll: " << roll << " >= " << dynConf.maxRoll << std::endl;
      }
      if (fabs(pitch) >= dynConf.maxPitch) {
        sstr << "pitch: " << pitch << " >= " << dynConf.maxPitch << std::endl;
      }

      // ROS_INFO("%s", sstr.str().c_str());
      std::cout << sstr.str() << std::endl;
    }
  }

}

} // namespace libobjecttracker
