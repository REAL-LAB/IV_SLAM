/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University 
of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/


#include<iostream>
#include<algorithm>
#include<fstream>
#include<iomanip>
#include<chrono>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <dirent.h>
#include <csignal>
#include<opencv2/core/core.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>


#include<System.h>
#include "io_access.h"

#if (CV_VERSION_MAJOR >= 4)
  #include<opencv2/imgcodecs/legacy/constants_c.h>
#endif


// Length of the image name suffix that should be extracted from its name
const int KImageNameSuffixLength = 10;

// Global variables
ORB_SLAM2::System *SLAM_ptr;


// Command line flags flag
DEFINE_string(vocab_path, "", "Path to ORB vocabulary.");
DEFINE_string(settings_path, "", "Path to ORB-SLAM config file.");
DEFINE_string(data_path, "", "Path to the source dataset.");
DEFINE_string(ground_truth_path, "", "Path to ground truth poses.");
DEFINE_string(img_qual_path, "", "Path to quality images for feature "
             "extraction/matching. Higher values of a pixel indicate lower "
             "reliability of the features extracted from that pixel in the "
             "image.");
DEFINE_string(out_visualization_path, "", "Output path for visualization "
                                          "results.");
DEFINE_string(out_dataset_path, "", "Output path for generated dataset.");
DEFINE_string(rel_pose_uncertainty_path, "", "Path to relative camera pose "
                                              "uncertainty values.");

DEFINE_int32(start_frame, 0, "Start frame ID.");
DEFINE_int32(end_frame, -1, "End frame ID.");

// If set to ture, the estimated camera pose uncertainty values are loaded 
// and passed to IV-SLAM
DEFINE_bool(load_rel_pose_uncertainty, false, "Loads relative camera pose "
                                           "uncertainty values from file.");

// Set to true if you would like to use predicted heatmaps the same size as
// the input images for weighting the extracted keypoints.
// NOTE: If the program is run in ivslam_enabled  and inference mode but this 
// is set to false, it is equivalent to running original ORB-SLAM with the 
// additional logging and visualization that is provided in ivslam_enabled mode
DEFINE_bool(load_img_qual_heatmaps, false, "Loads predicted image quality "
                                           "heatmpas from file.");

DEFINE_bool(run_single_threaded, false, "Runs in single threaded mode.");
DEFINE_bool(create_ivslam_dataset, false, "Saves to file the dataset for "
                                          "training the introspection model.");
DEFINE_bool(ivslam_enabled, false, "Enables IV-SLAM. The program will run "
               "in trainig mode unless the inference_mode flag is set.");
DEFINE_bool(inference_mode, false, "Enables the inference mode.");
DEFINE_bool(save_visualizations, false, "Saves visualization to file if in "
                                      "ivslam_enabled mode.");
DEFINE_bool(enable_viewer, true, "Enables the viewer.");

DECLARE_bool(help);
DECLARE_bool(helpshort);

using namespace std;

// Checks if all required command line arguments have been set
void CheckCommandLineArgs(char** argv) {
  vector<string> required_args = {"vocab_path",
                                  "settings_path",
                                  "data_path",
                                  "ground_truth_path",
                                  "out_visualization_path",
                                  "out_dataset_path"};
  
  for (const string& arg_name:required_args) {
    bool flag_not_set =   
          gflags::GetCommandLineFlagInfoOrDie(arg_name.c_str()).is_default;
    if (flag_not_set) {
      gflags::ShowUsageWithFlagsRestrict(argv[0], "stereo_kitti_opt");
      LOG(FATAL) << arg_name <<  " was not set." << endl;
    }
  }
}

void LoadImages(const string &strPathToSequence, vector<string> &vstrImageLeft,
                 vector<double> &vTimestamps);

// Loads images as well as the corresponding ground truth camera poses
void LoadImagesWithGT(const string &strPathToSequence,
                      const string &strPathToGroundTruth,
                      const string &strPathToImageQual,
                      const string &strPathToPoseUncertainty,
                      const bool &load_pose_uncertainty,
                      vector<string> &vstrImageLeft,
                      vector<string> &vstrImageQualFilenames,
                      vector<double> &vTimestamps, 
                      vector<cv::Mat>* cam_pose_gt,
                      vector<Eigen::Vector2f>* rel_cam_pose_uncertainty);

bool GetImageQualFileNames(const std::string &directory,
                           const int &size,
                           vector<string> *vstrImageQualFilenames,
                           int * num_qual_imgs_found);

void SignalHandler( int signal_num ) { 
   cout << "Interrupt signal is (" << signal_num << ").\n"; 
  
   // terminate program  
   if(SLAM_ptr) {
     SLAM_ptr->ShutdownMinimal();
  }
  
  cout << "Exiting the program!" << endl;
   
   exit(signal_num);   
} 

int main(int argc, char **argv)
{
    google::InstallFailureSignalHandler();
    google::InitGoogleLogging(argv[0]);
    FLAGS_stderrthreshold = 2;   // ERROR level logging.
    FLAGS_minloglevel = 1; // WARNING level
    FLAGS_colorlogtostderr = 1;  // Colored logging.
    FLAGS_logtostderr = true;    // Don't log to disk
    signal(SIGINT, SignalHandler);
    
    string usage("This program runs monocular ORB-SLAM on KITTI format "
                 "data with the option to run with IV-SLAM in inference mode "
                 "or generate training data for it. \n");
   
    usage += string(argv[0]) + " <argument1> <argument2> ...";
    gflags::SetUsageMessage(usage);
    
    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_help) {
      gflags::ShowUsageWithFlagsRestrict(argv[0], "mono_kitti");
      return 0;
    }
    CheckCommandLineArgs(argv);
   

    // Read rectification parameters
    cv::FileStorage fsSettings(FLAGS_settings_path, cv::FileStorage::READ);
    if(!fsSettings.isOpened())
    {
        cerr << "ERROR: Wrong path to settings" << endl;
        return -1;
    }


    
    // Retrieve paths to images
    vector<string> vstrImageLeft;
    vector<string> vstrDepthImageFilenames;
    vector<string> vstrImageQualFilenames;
    vector<double> vTimestamps;
    vector<cv::Mat> cam_poses_gt;
    vector<Eigen::Vector2f> rel_cam_poses_uncertainty;
    // The map from left image names to the ID of corresponding relative camera 
    // pose uncertainty (from current image to next image)
    std::unordered_map<std::string, int> pose_unc_map;
   
    if (!FLAGS_ivslam_enabled) {
      LoadImages(FLAGS_data_path, vstrImageLeft, vTimestamps);
    } else {
      LoadImagesWithGT(FLAGS_data_path, 
                       FLAGS_ground_truth_path,
                       FLAGS_img_qual_path,
                       FLAGS_rel_pose_uncertainty_path,
                       FLAGS_load_rel_pose_uncertainty,
                       vstrImageLeft,
                       vstrImageQualFilenames,
                       vTimestamps,
                       &cam_poses_gt,
                       &rel_cam_poses_uncertainty);
      
      if (FLAGS_load_rel_pose_uncertainty) {
        cout << "loading rel pose unc " << endl;
        for (size_t i = 0; i < vstrImageLeft.size(); i++) {
          string img_name_short = vstrImageLeft[i].substr(
                            vstrImageLeft[i].length() - KImageNameSuffixLength, 
                            KImageNameSuffixLength);
          pose_unc_map[img_name_short] = i;
        }
      }
    }

    const int nImages = vstrImageLeft.size();

    // Create SLAM system. It initializes all system threads and gets ready to 
    // process frames.
    bool use_BoW = true;
    bool silent_mode = false;
    bool guided_ba = false;
    ORB_SLAM2::System SLAM(FLAGS_vocab_path,
                           FLAGS_settings_path,
                           ORB_SLAM2::System::MONOCULAR,
                           FLAGS_enable_viewer,
                           FLAGS_ivslam_enabled,
                           FLAGS_inference_mode,
                           FLAGS_save_visualizations,
                           FLAGS_create_ivslam_dataset,
                           FLAGS_run_single_threaded,
                           use_BoW,
                           FLAGS_out_visualization_path,
                           FLAGS_out_dataset_path,
                           silent_mode,
                           guided_ba);
    if (FLAGS_load_rel_pose_uncertainty) {
      SLAM.SetRelativeCamPoseUncertainty(&pose_unc_map, 
                                        &rel_cam_poses_uncertainty);
    }
  
    SLAM_ptr = &SLAM;

    // Vector for tracking time statistics
    vector<float> vTimesTrack;
    vTimesTrack.resize(nImages);

    cout << endl << "-------" << endl;
    cout << "Start processing sequence ..." << endl;
    cout << "Images in the sequence: " << nImages << endl << endl;  
    cout << "Start frame: " << FLAGS_start_frame << endl;
      
    // Main loop
    cv::Mat imLeft;
    int end_frame;
    if(FLAGS_end_frame > 0) {
      end_frame = std::min(nImages, FLAGS_end_frame);
    } else {
      end_frame = nImages;
    }
    for(int ni=FLAGS_start_frame; ni < end_frame; ni++)
    {
#ifdef COMPILEDWITHC11
        std::chrono::steady_clock::time_point t1 = 
                        std::chrono::steady_clock::now();
#else
        std::chrono::monotonic_clock::time_point t1 = 
                        std::chrono::monotonic_clock::now();
#endif

        // Read left and right images from file
        imLeft = cv::imread(vstrImageLeft[ni],CV_LOAD_IMAGE_UNCHANGED);
        double tframe = vTimestamps[ni];

        if(imLeft.empty())
        {
            cerr << endl << "Failed to load image at: "
                 << string(vstrImageLeft[ni]) << endl;
            return 1;
        }
        

        
        // Read the predicted quality image
        cv::Mat qual_img;
        if (FLAGS_load_img_qual_heatmaps) {
          // There might not be a image quality available for all input 
          // images. In that case just skip the missing ones with setting 
          // them to empty images. (The SLAM object will ignore empty
          // images as if no score was available).
          if(vstrImageQualFilenames[ni].empty()) {
            qual_img = cv::Mat(0, 0, CV_8U);
          } else {
          
            // Read the predicted image quality
            qual_img = cv::imread(
                            vstrImageQualFilenames[ni],CV_LOAD_IMAGE_GRAYSCALE);
            if(qual_img.empty()) {
                cerr << endl << "Failed to load image at: " << 
                                vstrImageQualFilenames[ni] << endl;
                return 1;
            }
            
//             cv::imshow("heatmap", qual_img);
//             cv::waitKey(0);
          }
        }


        
        string img_name = vstrImageLeft[ni].substr(
                          vstrImageLeft[ni].length() - KImageNameSuffixLength, 
                          KImageNameSuffixLength);
        
        Eigen::Matrix<double, 6, 6> cam_poses_gt_cov;
        bool pose_cov_available = false;
        
        // Pass the images to the SLAM system
        if (FLAGS_ivslam_enabled) {
          SLAM.TrackMonocular(imLeft,
                           tframe, 
                           cam_poses_gt[ni],
                           img_name,
                           false,
                           qual_img);
        } else {
          SLAM.TrackMonocular(imLeft,tframe);
        }

#ifdef COMPILEDWITHC11
        std::chrono::steady_clock::time_point t2 = 
                  std::chrono::steady_clock::now();
#else
        std::chrono::monotonic_clock::time_point t2 = 
                  std::chrono::monotonic_clock::now();
#endif

        double ttrack= std::chrono::duration_cast<
                         std::chrono::duration<double>>(t2 - t1).count();

        vTimesTrack[ni]=ttrack;

        // Wait to load the next frame. In single threaded mode, load images
        // as fast as possible
        if(!FLAGS_run_single_threaded) {
          double T=0;
          if(ni<nImages-1)
              T = vTimestamps[ni+1]-tframe;
          else if(ni>0)
              T = tframe-vTimestamps[ni-1];

          if(ttrack<T)
              usleep((T-ttrack)*1e6);
        }
    }

    // Stop all threads
    SLAM.Shutdown();

    // Tracking time statistics
    sort(vTimesTrack.begin(),vTimesTrack.end());
    float totaltime = 0;
    for(int ni=0; ni<nImages; ni++)
    {
        totaltime+=vTimesTrack[ni];
    }
//     cout << "-------" << endl << endl;
//     cout << "median tracking time: " << vTimesTrack[nImages/2] << endl;
//     cout << "mean tracking time: " << totaltime/nImages << endl;

    cout << "--------------------" << endl;
    cout << "Finished processing sequence located at " 
         << FLAGS_data_path << endl;
    cout << "--------------------" << endl << endl;
       

    // Save camera trajectory -> currently being saved on shutdown under 
    // SLAM.Shutdown()
//     string path_to_cam_traj;
//     if (kSaveVisualizationsToFile) {
//       path_to_cam_traj = save_visualization_path + "/CameraTrajectory.txt";
//     } else {
//       path_to_cam_traj = "CameraTrajectory.txt";
//     }
//     SLAM.SaveTrajectoryKITTI(path_to_cam_traj);

    return 0;
}

void LoadImages(const string &strPathToSequence, vector<string> &vstrImageLeft,
                vector<double> &vTimestamps)
{
    ifstream fTimes;
    string strPathTimeFile = strPathToSequence + "/times.txt";
    fTimes.open(strPathTimeFile.c_str());
    while(!fTimes.eof())
    {
        string s;
        getline(fTimes,s);
        if(!s.empty())
        {
            stringstream ss;
            ss << s;
            double t;
            ss >> t;
            vTimestamps.push_back(t);
        }
    }

    string strPrefixLeft = strPathToSequence + "/image_0/";

    const int nTimes = vTimestamps.size();
    vstrImageLeft.resize(nTimes);

    for(int i=0; i<nTimes; i++)
    {
        stringstream ss;
        ss << setfill('0') << setw(6) << i;
        vstrImageLeft[i] = strPrefixLeft + ss.str() + ".png";
    }
}

void LoadImagesWithGT(const string &strPathToSequence,
                      const string &strPathToGroundTruth,
                      const string &strPathToImageQual,
                      const string &strPathToPoseUncertainty,
                      const bool &load_pose_uncertainty,
                      vector<string> &vstrImageLeft,
                      vector<string> &vstrImageQualFilenames,
                      vector<double> &vTimestamps, 
                      vector<cv::Mat>* cam_pose_gt,
                      vector<Eigen::Vector2f>* rel_cam_pose_uncertainty)
{   
    ifstream fTimes;
    string strPathTimeFile = strPathToSequence + "/times.txt";
    fTimes.open(strPathTimeFile.c_str());
    while(!fTimes.eof())
    {
        string s;
        getline(fTimes,s);
        if(!s.empty())
        {
            stringstream ss;
            ss << s;
            double t;
            ss >> t;
            vTimestamps.push_back(t);
        }
    }

    string strPrefixLeft = strPathToSequence + "/image_0/";

    const int nTimes = vTimestamps.size();
    vstrImageLeft.resize(nTimes);

    for(int i=0; i<nTimes; i++)
    {
        stringstream ss;
        ss << setfill('0') << setw(6) << i;
        vstrImageLeft[i] = strPrefixLeft + ss.str() + ".png";
    }
   
    // Load ground truth poses
    ifstream fGroundTruthPoses;
    fGroundTruthPoses.open(strPathToGroundTruth.c_str());
    while(!fGroundTruthPoses.eof()) {
        string s;
        getline(fGroundTruthPoses,s);
        if(!s.empty())
        {
            cv::Mat cam_pose = cv::Mat::eye(4, 4, CV_32F);
            stringstream ss(s);
            string str_curr_entry;
            for (size_t i = 0; i < 12; i++) {
                getline(ss, str_curr_entry, ' ');
                cam_pose.at<float>(floor((float)(i)/4), i%4) = 
                            stof(str_curr_entry);
            }
            cam_pose_gt->push_back(cam_pose);
        }
    }
    
    CHECK_EQ(cam_pose_gt->size(), nTimes);
   
    // Load reference pose uncertainty values 
    if (load_pose_uncertainty) {
      ifstream fRefPosesUnc;
      fRefPosesUnc.open(strPathToPoseUncertainty.c_str());
      while(!fRefPosesUnc.eof()) {
          string s;
          getline(fRefPosesUnc,s);
          if(!s.empty())
          {
              // pose_uncertainty: (translational_unc, rotational_unc)
              Eigen::Vector2f pose_uncertainty;
              stringstream ss(s);
              string str_curr_entry;
              for (size_t i = 0; i < 2; i++) {
                  getline(ss, str_curr_entry, ' ');
                  pose_uncertainty(i) = stof(str_curr_entry);
              }
              rel_cam_pose_uncertainty->push_back(pose_uncertainty);
          }
      }
      CHECK_EQ(rel_cam_pose_uncertainty->size(), nTimes);
    }
    
    
    // Load predicted quality images
    int qual_img_num = 0;
    if (FLAGS_load_img_qual_heatmaps) {
      if (!GetImageQualFileNames(strPathToImageQual,
                                nTimes,
                                &vstrImageQualFilenames,
                                &qual_img_num)){
        LOG(FATAL) << "Error loading the image quality files" << endl;
      }
      LOG(INFO) <<  qual_img_num << " predicted image quality heatmaps found.";
      
      if(qual_img_num != nTimes) {
        LOG(WARNING) << qual_img_num << " predicted image quality heatmaps "
                     << "were found but total session image count is "
                     << nTimes;
      }
    }
    
    if (FLAGS_load_img_qual_heatmaps && qual_img_num < 2) {
      LOG(FATAL) << "Predicted image quality heatmaps not found!";
    }
}


// Given a direcotry whose content is supposed to be files named as ID numbers 
// in the format %010d.jpg, it will fill vstrImageQualFilenames with full path 
// to all available files and leave missing images as empty strings. The 
// argument "size" determines how many images we are expecting starting at
// the index of 0
bool GetImageQualFileNames(const std::string &directory,
                           const int &size,
                           vector<string> *vstrImageQualFilenames,
                           int * num_qual_imgs_found) {
  vstrImageQualFilenames->clear();
  vstrImageQualFilenames->resize(size);
  
  const int kPrefixLength = 10;
  char numbering[10];
  *num_qual_imgs_found = 0;
  
  DIR* dirp = opendir(directory.c_str());
  struct dirent * dp;
  
  if(!dirp) {
    LOG(ERROR) << "Could not open directory " << directory 
               << " for predicted image quality heatmaps.";
  }

  while ((dp = readdir(dirp)) != NULL){

    // Ignore the '.' and ".." directories
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;
    for(int i = 0; i < kPrefixLength ; i++){
        numbering[i] = dp->d_name[i];
    }
   
    int prefix_number = atoi(numbering);
   
    CHECK_LT(prefix_number, size) << "The index of the images are expected to"
                                  << "be between 0 and " << size - 1;
    vstrImageQualFilenames->at(prefix_number) = directory + "/" +
                                                std::string(dp->d_name);
    *num_qual_imgs_found = *num_qual_imgs_found + 1;
  }
  (void)closedir(dirp);
  
  return true;
}
