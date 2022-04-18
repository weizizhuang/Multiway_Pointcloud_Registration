#ifndef REGISTRATION_PIPELINE_H
#define REGISTRATION_PIPELINE_H
#include "prereq.h"

#include <vector>
#include <string>


namespace RegPipeline
{
	// Relocate point cloud using GPS data
	// filenames[0]: input filename of point cloud
	// filenames[1]: local coordinates of cameras
	// filenames[2]: global coordinates of cameras
	// filenames[3]: output filename of transformed point cloud
	REGPIPELINE_PUBLIC void LocalizePointCloud(const std::vector<std::string> &filenames, std::string &format);

	// Trim two points clouds
	// filenames[0]: input filename of point cloud a
	// filenames[1]: input filename of point cloud b
	// filenames[2]: output filename of point cloud a
	// filenames[3]: output filename of point cloud b
	REGPIPELINE_PUBLIC void TrimPointsClouds(const std::vector<std::string> &filenames, std::string &format);

	// Point cloud registration using Go-ICP
	// config_filename: input filename of config xml file
	REGPIPELINE_PUBLIC void PointCloudRegistrationUsingGoICP(const std::string& config_filename);

	// Point cloud registration using our modified Scale-FGR
	// config_filename: input filename of config xml file


	REGPIPELINE_PUBLIC void PointCloudRegistrationUsingScaleFGR(const std::string& config_filename);

	REGPIPELINE_PUBLIC void PointCloudRegistrationUsingScaleMultiway(const std::string& config_filename);


	// Multi-way point cloud registration using our modified Scale-FGR	
	// config_filename: input filename of config xml file
	REGPIPELINE_PUBLIC void MultiwayPointCloudRegistrationUsingScaleFGR(const std::string& config_filename);


}

#endif