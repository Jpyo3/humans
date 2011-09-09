/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011, Georgia Institute of Technology
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Georgia Institute of Technology nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/
// ROS
#include <ros/ros.h>
#include <ros/package.h>
#include <actionlib/server/simple_action_server.h>

#include <std_msgs/Header.h>
#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/CameraInfo.h>

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/CvBridge.h>
#include <cv_bridge/cv_bridge.h>

// TF
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>

// PCL
#include <pcl/ros/conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_ros/transforms.h>
#include <pcl/ModelCoefficients.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/features/feature.h>
#include <pcl/common/eigen.h>
#include "pcl/filters/voxel_grid.h"
#include <pcl/filters/passthrough.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/surface/convex_hull.h>

// OpenCV
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/features2d/features2d.hpp>

// tabletop_pushing
#include <tabletop_pushing/PushPose.h>
#include <tabletop_pushing/LocateTable.h>
#include <tabletop_pushing/SegTrackAction.h>

#include <tabletop_pushing/extern/graphcut/graph.h>
#include <tabletop_pushing/extern/graphcut/energy.h>
#include <tabletop_pushing/extern/graphcut/GCoptimization.h>

// STL
#include <vector>
#include <deque>
#include <queue>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <utility>
#include <math.h>

// Debugging IFDEFS
// #define DISPLAY_INPUT_COLOR 1
// #define DISPLAY_INPUT_DEPTH 1
// #define DISPLAY_WORKSPACE_MASK 1
// #define DISPLAY_MOTION_PROBS 1
// #define DISPLAY_MEANS 1
// #define DISPLAY_VARS 1
// #define DISPLAY_INTERMEDIATE_PROBS 1
// #define DISPLAY_OPTICAL_FLOW 1
// #define DISPLAY_UV 1
// #define DISPLAY_OPT_FLOW_INTERNALS 1
// #define DISPLAY_GRAPHCUT 1
// #define VISUALIZE_GRAPH_WEIGHTS 1
// #define DISPLAY_ARM_PROBS 1
// #define DISPLAY_ARM_CIRCLES 1

// #define WRITE_INPUT_TO_DISK 1
// #define WRITE_CUTS_TO_DISK 1
// #define WRITE_FLOWS_TO_DISK 1
// #define WRITE_ARMS_TO_DISK 1

// Functional IFDEFS
// #define REMOVE_SMALL_BLOBS

using tabletop_pushing::PushPose;
using tabletop_pushing::LocateTable;
using geometry_msgs::PoseStamped;
using geometry_msgs::PointStamped;
typedef pcl::PointCloud<pcl::PointXYZ> XYZPointCloud;
typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image,
                                                        sensor_msgs::Image,
                                                        sensor_msgs::PointCloud2> MySyncPolicy;
typedef Graph<float, float, float> GraphType;


void displayOpticalFlow(cv::Mat& color_frame, cv::Mat& flow_u, cv::Mat& flow_v,
                        float mag_thresh)
{
  cv::Mat flow_thresh_disp_img(color_frame.size(), CV_8UC3);
  flow_thresh_disp_img = color_frame.clone();
  // float corner_thresh = (eigen_ratio_+1)*(eigen_ratio_+1)/eigen_ratio_;
  for (int r = 0; r < flow_thresh_disp_img.rows; ++r)
  {
    for (int c = 0; c < flow_thresh_disp_img.cols; ++c)
    {
      float u = flow_u.at<float>(r,c);
      float v = flow_u.at<float>(r,c);
      if (std::sqrt(u*u+v*v) > mag_thresh)
      {
        cv::line(flow_thresh_disp_img, cv::Point(c,r), cv::Point(c-u, r-v),
                 cv::Scalar(0,255,0));
        // cv::circle(flow_thresh_disp_img, cv::Point(c,r), 2, cv::Scalar(0,255,0));
      }
    }
  }
  std::vector<cv::Mat> flows;
  cv::imshow("flow_disp", flow_thresh_disp_img);
#ifdef DISPLAY_UV
  cv::imshow("u", flow_u);
  cv::imshow("v", flow_v);
#endif // DISPLAY_UV
}

class ProbImageDifferencing
{
 public:
  ProbImageDifferencing(unsigned int num_hist_frames=5, float T_in=3,
                        float T_out=15) :
      num_hist_frames_(num_hist_frames), ONES(1.0, 1.0, 1.0, 1.0),
      T_in_(T_in), T_out_(T_out)
  {
    g_kernel_ = cv::getGaussianKernel(3, 2.0, CV_32F);
  }

  void initialize(cv::Mat& color_frame, cv::Mat& depth_frame)
  {
    // Restart the motion history
    motion_probs_.create(color_frame.size(), CV_32FC4);
    d_motion_probs_.create(depth_frame.size(), CV_32FC1);

    // Restart the queue
    pixel_histories_.clear();
    d_histories_.clear();
    update(color_frame, depth_frame);
  }

  // std::vector<cv::Mat> update(cv::Mat& bgrd_frame)
  std::vector<cv::Mat> update(cv::Mat& color_frame, cv::Mat& depth_frame_pre_blur)
  {
    // Convert to correct format
    cv::Mat bgr_frame_pre_blur(color_frame.size(), CV_32FC3);
    color_frame.convertTo(bgr_frame_pre_blur, CV_32FC3, 1.0/255, 0);
    cv::Mat bgr_frame(color_frame.size(), CV_32FC3);
    cv::Mat depth_frame(color_frame.size(), CV_32FC3);
    // Smooth images before using
    cv::filter2D(bgr_frame_pre_blur, bgr_frame, CV_32F, g_kernel_);
    cv::filter2D(depth_frame_pre_blur, depth_frame, CV_32F, g_kernel_);

    // Calculate probability of being the same color and depth at each pixel
    std::vector<cv::Mat> motions;
    if (pixel_histories_.size() > 0)
      motions = calculateProbabilities(bgr_frame, depth_frame);

    // Store current frame in history
    pixel_histories_.push_back(bgr_frame);
    d_histories_.push_back(depth_frame);

    // Pop the front of the queue if we have too many frames
    if (pixel_histories_.size() > num_hist_frames_)
    {
      pixel_histories_.pop_front();
      d_histories_.pop_front();
    }

    return motions;
  }

  std::vector<cv::Mat> calculateProbabilities(cv::Mat& bgr_frame,
                                              cv::Mat& depth_frame)
  {
    // Update Gaussian estimates at each pixel
    // Calculate means
    cv::Mat means(bgr_frame.size(), CV_32FC3, cv::Scalar(0.0,0.0,0.0));
    cv::Mat d_means(depth_frame.size(), CV_32FC1, cv::Scalar(0.0));
    for (unsigned int i = 0; i < pixel_histories_.size(); ++i)
    {
      for (int r = 0; r < means.rows; ++r)
      {
        for (int c = 0; c < means.cols; ++c)
        {
          means.at<cv::Vec3f>(r,c) += pixel_histories_[i].at<cv::Vec3f>(r,c);
          d_means.at<float>(r,c) += d_histories_[i].at<float>(r,c);
        }
      }
    }
    means /= pixel_histories_.size();
    d_means /= d_histories_.size();

#ifdef DISPLAY_MEANS
    std::vector<cv::Mat> means_vec;
    cv::split(means, means_vec);

    cv::imshow("means_h", means_vec[0]);
    cv::imshow("means_s", means_vec[1]);
    cv::imshow("means_v", means_vec[2]);
    cv::imshow("d_means", d_means);
#endif // DISPLAY_MEANS

    // Calculate variances
    cv::Mat vars(bgr_frame.size(), CV_32FC3, cv::Scalar(0.0,0.0,0.0));
    cv::Mat d_vars(depth_frame.size(), CV_32FC1, cv::Scalar(0.0));
    for (unsigned int i = 0; i < pixel_histories_.size(); ++i)
    {
      for (int r = 0; r < means.rows; ++r)
      {
        for (int c = 0; c < means.cols; ++c)
        {
          cv::Vec3f diff = pixel_histories_[i].at<cv::Vec3f>(r,c) -
              means.at<cv::Vec3f>(r,c);

          vars.at<cv::Vec3f>(r,c) += diff.mul(diff);

          float d_diff = d_histories_[i].at<float>(r,c)-d_means.at<float>(r,c);
          d_vars.at<float>(r,c) += d_diff * d_diff;
        }
      }
    }
    if (pixel_histories_.size() > 1)
    {
      vars /= (pixel_histories_.size()-1.0);
      d_vars /= (d_histories_.size()-1.0);
    }
    else
    {
      // pass
    }

#ifdef DISPLAY_VARS
    std::vector<cv::Mat> vars_vec;
    cv::split(vars, vars_vec);
    cv::imshow("vars_h", vars_vec[0]);
    cv::imshow("vars_s", vars_vec[1]);
    cv::imshow("vars_v", vars_vec[2]);
    cv::imshow("d_vars", d_vars);
#endif // DISPLAY_VARS

    int d_probs_greater = 0;
    int d_probs_lesser = 0;
    // Calculate probability of pixels having moved
    for (int r = 0; r < motion_probs_.rows; ++r)
    {
      for (int c = 0; c < motion_probs_.cols; ++c)
      {
        cv::Vec3f x = bgr_frame.at<cv::Vec3f>(r,c);
        cv::Vec3f mu = means.at<cv::Vec3f>(r,c);
        cv::Vec3f var = vars.at<cv::Vec3f>(r,c);
        // NOTE: Probability of not belonging to the gaussian
        // cv::Vec4f probs = ONES - p_x_gaussian(x, mu, var);
        cv::Vec4f probs = p_x_fitzpatrick(x, mu, var);
        motion_probs_.at<cv::Vec4f>(r,c) = probs;
        float x_d = depth_frame.at<float>(r,c);
        float mu_d = d_means.at<float>(r,c);
        float var_d = d_vars.at<float>(r,c);
        // float prob_d = 1.0 - p_x_gaussian_kenney(x_d, mu_d, var_d);
        float prob_d = p_x_fitzpatrick(x_d, mu_d, var_d);
        if (prob_d > 1.0)
        {
          d_probs_greater++;
        }
        else if (prob_d < 0.0)
        {
          d_probs_lesser++;
        }
        d_motion_probs_.at<float>(r,c) = prob_d;
        // motion_probs_.at<cv::Vec4f>(r,c)[3]*= prob_d;
        motion_probs_.at<cv::Vec4f>(r,c)[3]= max(prob_d,
                                                 motion_probs_.at<cv::Vec4f>(r,c)[3]);
      }
    }

    // TODO: Merge these into a single image?
    std::vector<cv::Mat> motions;
    motions.push_back(motion_probs_);
    motions.push_back(d_motion_probs_);
    return motions;
  }

  cv::Vec4f p_x_gaussian(cv::Vec3f x, cv::Vec3f mu, cv::Vec3f var)
  {
    cv::Vec4f p_x;
    p_x[0] = p_x_gaussian_kenney(x[0], mu[0], var[0]);
    p_x[1] = p_x_gaussian_kenney(x[1], mu[1], var[1]);
    p_x[2] = p_x_gaussian_kenney(x[2], mu[2], var[2]);
    p_x[3] = p_x[0]*p_x[1]*p_x[2];
    // p_x[3] = max(p_x[0],max(p_x[1],p_x[2]));
    return p_x;
  }

  cv::Vec4f p_x_fitzpatrick(cv::Vec3f x, cv::Vec3f mu, cv::Vec3f var)
  {
    cv::Vec4f p_x;
    p_x[0] = p_x_fitzpatrick(x[0], mu[0], var[0]);
    p_x[1] = p_x_fitzpatrick(x[1], mu[1], var[1]);
    p_x[2] = p_x_fitzpatrick(x[2], mu[2], var[2]);
    p_x[3] = p_x[0]*p_x[1]*p_x[2];
    // p_x[3] = max(p_x[0],max(p_x[1],p_x[2]));
    return p_x;
  }

  inline float p_x_fitzpatrick(const float x, const float mu, float var)
  {
    // float new_var = 5.0;
    if (isnan(var))
    {
      var = 0.1;
    }
    return fabs(x-mu)/(var+0.1);
  }

  float p_x_gaussian_kenney(float x, float mu, float var)
  {
    float mean_diff = abs(x-mu);
    float sigma = sqrt(var);
    float s0 = T_in_*sigma;
    float s1 = T_out_*sigma;

    if (mean_diff <= s0)
    {
      // Don't make things impossible (i.e. depth does not change when things
      // are too close)
      return 0.99;
      // return 1.0;
    }
    if (mean_diff >= s1)
    {
      return 0.01;
    }
    float m = 1.0/(s0-s1);
    return m*(mean_diff-s1);
  }


  float p_x_gaussian_hist(float x, float mu, float var)
  {
    // TODO: Figure out a better way of approximating width
    float x1 = x-0.05;
    float x2 = x+0.05;
    float p_x = (abs(gaussian_pdf(x2, mu, var) - gaussian_pdf(x1, mu, var))*
                 abs(x2-x1));
    return p_x;
  }

  float gaussian_pdf(float x, float mu, float var)
  {
    if (var == 0.0)
    {
      // Don't make things impossible
      if (x == mu)
      {
        return 0.99;
        // return 1.0;
      }
      else
        return 0.01;
    }
    float mean_diff = x-mu;
    return exp(-(mean_diff*mean_diff)/(2.0*var))/(sqrt(2.0*var*M_PI));
  }

  void setNumHistFrames(unsigned int num_hist_frames)
  {
    num_hist_frames_ = num_hist_frames;
  }
  void setT_inT_out(float T_in, float T_out)
  {
    T_in_ = T_in;
    T_out_ = T_out;
  }

 protected:
  cv::Mat motion_probs_;
  cv::Mat d_motion_probs_;
  std::deque<cv::Mat> pixel_histories_;
  std::deque<cv::Mat> d_histories_;
  unsigned int num_hist_frames_;
  const cv::Vec4f ONES;
  float T_in_;
  float T_out_;
  cv::Mat g_kernel_;
};

class LKFlowReliable
{
 public:
  LKFlowReliable(int win_size = 5, int num_levels = 4) :
      win_size_(win_size), max_level_(num_levels-1)
  {
    // Create derivative kernels for flow calculation
    cv::getDerivKernels(dy_kernel_, dx_kernel_, 1, 0, CV_SCHARR, true, CV_32F);
    // TODO: Make directly
    // [+3 +10 +3
    //   0   0  0
    //  -3 -10 -3]
    cv::flip(dy_kernel_, dy_kernel_, -1);
    g_kernel_ = cv::getGaussianKernel(3, 2.0, CV_32F);
    optic_g_kernel_ = cv::getGaussianKernel(5, 10.0, CV_32F);
    cv::transpose(dy_kernel_, dx_kernel_);
  }

  virtual ~LKFlowReliable()
  {
  }

  std::vector<cv::Mat> operator()(cv::Mat& cur_color_frame,
                                  cv::Mat& prev_color_frame)
  {
    // Convert to grayscale
    cv::Mat tmp_bw(cur_color_frame.size(), CV_8UC1);
    cv::Mat cur_bw(cur_color_frame.size(), CV_32FC1);
    cv::Mat prev_bw(prev_color_frame.size(), CV_32FC1);
    cv::cvtColor(cur_color_frame, tmp_bw, CV_BGR2GRAY);
    tmp_bw.convertTo(cur_bw, CV_32FC1, 1.0/255, 0);
    cv::cvtColor(prev_color_frame, tmp_bw, CV_BGR2GRAY);
    tmp_bw.convertTo(prev_bw, CV_32FC1, 1.0/255, 0);

    std::vector<cv::Mat> flow_n_scores = hierarchy(cur_bw, prev_bw);
    return flow_n_scores;
  }

  std::vector<cv::Mat> hierarchy(cv::Mat& f2, cv::Mat& f1)
  {
    const int divisor = std::pow(2,max_level_);
    cv::Mat Dx(f2.rows/divisor, f2.cols/divisor, CV_32FC1, cv::Scalar(0.0));
    cv::Mat Dy = Dx.clone();
    std::vector<cv::Mat> g1s;
    std::vector<cv::Mat> g2s;
    cv::buildPyramid(f1, g1s, max_level_);
    cv::buildPyramid(f2, g2s, max_level_);
    std::vector<cv::Mat> flow_outs;

    for (int l = max_level_; l >= 0; --l)
    {
      if (l != max_level_)
      {
        Dx = expand(Dx);
        Dy = expand(Dy);
      }
      cv::Mat W = warp(g1s[l], Dx, Dy);
      flow_outs = baseLK(g2s[l], W);
      Dx = Dx + flow_outs[0];
      Dy = Dy + flow_outs[1];
      Dx = smooth(Dx);
      Dy = smooth(Dy);
    }
    std::vector<cv::Mat> total_outs;
    total_outs.push_back(Dx);
    total_outs.push_back(Dy);
    total_outs.push_back(flow_outs[2]);
    return total_outs;
  }

  // TODO: Clean up this method to be more like optic.m
  std::vector<cv::Mat> baseLK(cv::Mat& cur_bw, cv::Mat& prev_bw)
  {
    cv::Mat Ix(cur_bw.size(), CV_32FC1);
    cv::Mat Iy(cur_bw.size(), CV_32FC1);
    cv::Mat cur_blur(cur_bw.size(), cur_bw.type());
    cv::Mat prev_blur(prev_bw.size(), prev_bw.type());
    cv::filter2D(cur_bw, cur_blur, CV_32F, g_kernel_);
    cv::filter2D(prev_bw, prev_blur, CV_32F, g_kernel_);
    cv::Mat It = cur_blur - prev_blur;

    // Get image derivatives
    cv::filter2D(cur_bw, Ix, CV_32F, dx_kernel_);
    cv::filter2D(cur_bw, Iy, CV_32F, dy_kernel_);

#ifdef DISPLAY_OPT_FLOW_INTERNALS
    cv::imshow("cur_bw", cur_bw);
    cv::imshow("prev_bw", prev_bw);
    cv::imshow("It", It);
    cv::imshow("Ix", Ix);
    cv::imshow("Iy", Iy);
#endif // DISPLAY_OPT_FLOW_INTERNALS

    int win_radius = win_size_/2;
    cv::Mat flow_u(cur_bw.size(), CV_32FC1, cv::Scalar(0.0));
    cv::Mat flow_v(cur_bw.size(), CV_32FC1, cv::Scalar(0.0));
    cv::Mat t_scores(cur_bw.size(), CV_32FC1, cv::Scalar(0.0));
    for (int r = win_radius; r < Ix.rows-win_radius; ++r)
    {
      for (int c = win_radius; c < Ix.cols-win_radius; ++c)
      {
        float sIxx = 0.0;
        float sIyy = 0.0;
        float sIxy = 0.0;
        float sIxt = 0.0;
        float sIyt = 0.0;
        for (int y = r-win_radius; y <= r+win_radius; ++y)
        {
          for (int x = c-win_radius; x <= c+win_radius; ++x)
          {
            sIxx += Ix.at<float>(y,x)*Ix.at<float>(y,x);
            sIyy += Iy.at<float>(y,x)*Iy.at<float>(y,x);
            sIxy += Ix.at<float>(y,x)*Iy.at<float>(y,x);
            sIxt += Ix.at<float>(y,x)*It.at<float>(y,x);
            sIyt += Iy.at<float>(y,x)*It.at<float>(y,x);
          }
        }

        float det = sIxx*sIyy - sIxy*sIxy;
        cv::Vec2f uv;
        if (det == 0.0)
        {
          uv[0] = 0.0;
          uv[1] = 0.0;
        }
        else
        {
          uv[0] = (-sIyy*sIxt + sIxy*sIyt)/det;
          uv[1] = (sIxy*sIxt - sIxx*sIyt)/det;
        }
        flow_u.at<float>(r,c) = uv[0];
        flow_v.at<float>(r,c) = uv[1];
        t_scores.at<float>(r,c) = (sIxx+sIyy)*(sIxx+sIyy)/(det);
      }
    }
    std::vector<cv::Mat> outs;
    outs.push_back(flow_u);
    outs.push_back(flow_v);
    outs.push_back(t_scores);
    return outs;
  }

  cv::Mat reduce(cv::Mat& input /*, cv::Mat kernel*/)
  {
    cv::Mat output;// = input.clone();
    cv::pyrDown(input, output);
    return output;
  }

  cv::Mat expand(cv::Mat& input /*, cv::Mat kernel*/)
  {
    cv::Mat output(input.rows*2, input.cols*2, CV_32FC1);
    cv::pyrUp(input, output, output.size());
    return output;
  }

  cv::Mat smooth(cv::Mat& input, int n=1)
  {
    cv::Mat sm = input.clone();
    for (int l = 0; l < n; ++l)
    {
      sm = reduce(sm);
    }
    for (int l = 0; l < n; ++l)
    {
      sm = expand(sm);
    }
    return sm;
  }

  cv::Mat warp(cv::Mat& i2, cv::Mat& vx, cv::Mat& vy)
  {
    cv::Mat warpI2(i2.rows, i2.cols, i2.type(), cv::Scalar(0.0));
    // cv::Mat warpI3(i2.rows, i2.cols, i2.type(), cv::Scalar(0.0));
    cv::Mat map_x(vx.rows, vx.cols, CV_32FC1);
    cv::Mat map_y(vy.rows, vy.cols, CV_32FC1);

    for (int y = 0; y < map_x.rows; ++y)
    {
      for (int x = 0; x < map_x.cols; ++x)
      {
        // TODO: Check image ordering forward/inverse stuff
        map_x.at<float>(y,x) = x + vx.at<float>(y,x);
        map_y.at<float>(y,x) = y + vy.at<float>(y,x);
      }
    }

    // cv::remap(i2, warpI3, map_x, map_y, cv::INTER_NEAREST);
    cv::remap(i2, warpI2, map_x, map_y, cv::INTER_LINEAR);

    // for (int y = 0; y < i2.rows; ++y)
    // {
    //   for (int x = 0; x < i2.cols; ++x)
    //   {
    //     if (isnan(warpI2.at<float>(y,x)))
    //     {
    //       warpI2.at<float>(y,x) = warpI3.at<float>(y,x);
    //     }
    //   }
    // }
    return warpI2;
  }

  void integralImage(cv::Mat& input)
  {
    // Compute integral image first row of values
    double* row0 = input.ptr<double>(0);
    for (int c = 1; c < input.cols; ++c)
    {
      row0[c] += row0[c-1];
    }

    // Compute integral image first column of values
    for (int r = 1; r < input.rows; ++r)
    {
      input.at<double>(r,0) += input.at<double>(r-1,0);
    }

    // Compute integral image values
    double* prev_row = row0;
    double* cur_row;
    for (int r = 1; r < input.rows; ++r)
    {
      cur_row = input.ptr<double>(r);
      for (int c = 1; c < input.cols; ++c)
      {
        cur_row[c] += cur_row[c-1] + prev_row[c] - prev_row[c-1];
      }
      prev_row = cur_row;
    }
  }

  inline double filterIntegral(cv::Mat& integral, int x, int y, int radius)
  {
    int y_min = y - radius - 1;
    int x_min = x - radius - 1;
    int y_max = y + radius;
    int x_max = x + radius;

    double filter_response = integral.at<double>(y_max, x_max);
    if (x_min >= 0)
    {
      filter_response -= integral.at<double>(y_max, x_min);
    }
    if (y_min >= 0)
    {
      filter_response -= integral.at<double>(y_min, x_max);
    }
    if (x_min >= 0 && y_min >= 0)
    {
      filter_response += integral.at<double>(y_min, x_min);
    }
    return filter_response;
  }

  std::vector<cv::Mat> baseLKII(cv::Mat& cur_bw, cv::Mat& prev_bw)
  {

    // Blur the input images and get the difference
    cv::Mat cur_blur(cur_bw.size(), cur_bw.type());
    cv::Mat prev_blur(prev_bw.size(), prev_bw.type());
    cv::filter2D(cur_bw, cur_blur, CV_32F, g_kernel_);
    cv::filter2D(prev_bw, prev_blur, CV_32F, g_kernel_);
    cv::Mat It32 = cur_blur - prev_blur;

    // Get image derivatives
    cv::Mat Ix32(cur_bw.size(), CV_32FC1);
    cv::Mat Iy32(cur_bw.size(), CV_32FC1);
    cv::filter2D(cur_bw, Ix32, CV_32F, dx_kernel_);
    cv::filter2D(cur_bw, Iy32, CV_32F, dy_kernel_);

    // Convert images to 64 bit to preserve precision in integration
    cv::Mat Ix(cur_bw.size(), CV_64FC1);
    cv::Mat Iy(cur_bw.size(), CV_64FC1);
    cv::Mat It(It32.size(), CV_64FC1);
    Ix32.convertTo(Ix, CV_64FC1);
    Iy32.convertTo(Iy, CV_64FC1);
    It32.convertTo(It, CV_64FC1);

    // Create the integral images
    cv::Mat IIxx(cur_bw.size(), CV_64FC1);
    cv::Mat IIyy(cur_bw.size(), CV_64FC1);
    cv::Mat IIxy(cur_bw.size(), CV_64FC1);
    cv::Mat IIxt(cur_bw.size(), CV_64FC1);
    cv::Mat IIyt(cur_bw.size(), CV_64FC1);
    cv::multiply(Ix, Ix, IIxx);
    cv::multiply(Iy, Iy, IIyy);
    cv::multiply(Ix, Iy, IIxy);
    cv::multiply(Ix, It, IIxt);
    cv::multiply(Iy, It, IIyt);
    integralImage(IIxx);
    integralImage(IIyy);
    integralImage(IIxy);
    integralImage(IIxt);
    integralImage(IIyt);

#ifdef DISPLAY_OPT_FLOW_INTERNALS
    cv::imshow("IIxx", IIxx);
    cv::imshow("IIxy", IIxy);
    cv::imshow("IIyy", IIyy);
    cv::imshow("IIxt", IIxt);
    cv::imshow("IIyt", IIyt);
    cv::imshow("cur_bw", cur_blur);
    cv::imshow("prev_bw", prev_blur);
    cv::imshow("It", It);
    cv::imshow("Ix", Ix);
    cv::imshow("Iy", Iy);
#endif // DISPLAY_OPT_FLOW_INTERNALS

    // Compute the LK flow densely using the integral images
    int win_radius = win_size_/2;
    cv::Mat flow(cur_bw.size(), CV_64FC2, cv::Scalar(0.0,0.0));
    cv::Mat t_scores(cur_bw.size(), CV_32FC1, cv::Scalar(0.0,0.0));
    for (int r = win_radius; r < Ix.rows-win_radius; ++r)
    {
      for (int c = win_radius; c < Ix.cols-win_radius; ++c)
      {
        const double sIxx = filterIntegral(IIxx, c, r, win_radius);
        const double sIyy = filterIntegral(IIyy, c, r, win_radius);
        const double sIxy = filterIntegral(IIxy, c, r, win_radius);
        const double sIxt = filterIntegral(IIxt, c, r, win_radius);
        const double sIyt = filterIntegral(IIyt, c, r, win_radius);
        float det = sIxx*sIyy - sIxy*sIxy;
        cv::Vec2f uv;
        if (det == 0.0)
        {
          uv[0] = 0.0;
          uv[1] = 0.0;
        }
        else
        {
          uv[0] = (-sIyy*sIxt + sIxy*sIyt)/det;
          uv[1] = (sIxy*sIxt - sIxx*sIyt)/det;
        }
        flow.at<cv::Vec2f>(r,c) = uv;
        t_scores.at<float>(r,c) = (sIxx+sIyy)*(sIxx+sIyy)/(det);
      }
    }
    std::vector<cv::Mat> outs;
    outs.push_back(flow);
    outs.push_back(t_scores);
    return outs;
  }

  //
  // Getters and Setters
  //
  void setWinSize(int win_size)
  {
    win_size_ = win_size;
  }

  void setNumLevels(int num_levels)
  {
    max_level_ = num_levels-1;
  }

 protected:
  int win_size_;
  int max_level_;
  cv::Mat dx_kernel_;
  cv::Mat dy_kernel_;
  cv::Mat g_kernel_;
  cv::Mat optic_g_kernel_;
};

class MotionGraphcut
{
 public:
  MotionGraphcut(double eigen_ratio = 5.0f, double w_f = 3.0f, double w_b = 2.0f,
                 double w_n_f = 0.01f, double w_n_b = 0.01f, double w_w_b = 5.0f,
                 double w_u_f = 0.1f, double w_u_b = 0.1f,
                 double magnitude_thresh=0.1, int arm_grow_radius=2) :
      w_f_(w_f), w_b_(w_b), w_n_f_(w_n_f), w_n_b_(w_n_b),  w_w_b_(w_w_b),
      w_u_f_(w_u_f), w_u_b_(w_u_b), magnitude_thresh_(magnitude_thresh),
      arm_grow_radius_(arm_grow_radius)
  {
    setEigenRatio(eigen_ratio);
  }

  virtual ~MotionGraphcut()
  {
  }

  cv::Mat operator()(cv::Mat& color_frame, cv::Mat& depth_frame,
                     cv::Mat& u, cv::Mat& v, cv::Mat flow_scores,
                     cv::Mat& workspace_mask, std::vector<cv::Point> arm_locs)
  {
    const int R = color_frame.rows;
    const int C = color_frame.cols;
    int num_nodes = R*C;
    int num_edges = ((C-1)*3+1)*(R-1)+(C-1);
    g = new GraphType(num_nodes, num_edges);

#ifdef VISUALIZE_GRAPH_WEIGHTS
    cv::Mat fg_weights(color_frame.size(), CV_32FC1);
    cv::Mat bg_weights(color_frame.size(), CV_32FC1);
#endif // VISUALIZE_GRAPH_WEIGHTS

    for (int r = 0; r < R; ++r)
    {
      for (int c = 0; c < C; ++c)
      {
        g->add_node();
        float magnitude = std::sqrt(u.at<float>(r,c)*u.at<float>(r,c) +
                                    v.at<float>(r,c)*v.at<float>(r,c));

        // Check if we are hardcoding this spot to background
        if (workspace_mask.at<uchar>(r,c) == 0)
        {
          g->add_tweights(r*C+c, /*capacities*/ 0.0, w_w_b_);

#ifdef VISUALIZE_GRAPH_WEIGHTS
          fg_weights.at<float>(r,c) = 0.0;
          bg_weights.at<float>(r,c) = w_w_b_;
#endif // VISUALIZE_GRAPH_WEIGHTS
        }
        else if (flow_scores.at<float>(r,c) < corner_thresh_)
        {

          if (magnitude > magnitude_thresh_)
          {
            g->add_tweights(r*C+c, /*capacities*/ w_f_, w_n_f_);

#ifdef VISUALIZE_GRAPH_WEIGHTS
            fg_weights.at<float>(r,c) = w_f_;
            bg_weights.at<float>(r,c) = w_n_f_;
#endif // VISUALIZE_GRAPH_WEIGHTS
          }
          else
          {
            g->add_tweights(r*C+c, /*capacities*/ w_n_b_, w_b_);

#ifdef VISUALIZE_GRAPH_WEIGHTS
            fg_weights.at<float>(r,c) = w_n_b_;
            bg_weights.at<float>(r,c) = w_b_;
#endif // VISUALIZE_GRAPH_WEIGHTS
          }
        }
        else
        {
          g->add_tweights(r*C+c, /*capacities*/ w_u_f_, w_u_b_);
#ifdef VISUALIZE_GRAPH_WEIGHTS
          fg_weights.at<float>(r,c) = w_u_f_;
          bg_weights.at<float>(r,c) = w_u_b_;
#endif // VISUALIZE_GRAPH_WEIGHTS
        }

        // Connect node to previous ones
        if (c > 0)
        {
          // Add left-link
          float w_l = getEdgeWeight(color_frame.at<cv::Vec3f>(r,c),
                                    depth_frame.at<float>(r,c),
                                    color_frame.at<cv::Vec3f>(r,c-1),
                                    depth_frame.at<float>(r,c-1));
          g->add_edge(r*C+c, r*C+c-1, /*capacities*/ w_l, w_l);
        }

        if (r > 0)
        {
          // Add up-link
          float w_u = getEdgeWeight(color_frame.at<cv::Vec3f>(r,c),
                                    depth_frame.at<float>(r,c),
                                    color_frame.at<cv::Vec3f>(r-1,c),
                                    depth_frame.at<float>(r-1,c));
          g->add_edge(r*C+c, (r-1)*C+c, /*capacities*/ w_u, w_u);
          // Add up-left-link
          if (c > 0)
          {
            float w_ul = getEdgeWeight(color_frame.at<cv::Vec3f>(r,c),
                                       depth_frame.at<float>(r,c),
                                       color_frame.at<cv::Vec3f>(r-1,c-1),
                                       depth_frame.at<float>(r-1,c-1));
            g->add_edge(r*C+c, (r-1)*C+c-1, /*capacities*/ w_ul, w_ul);
          }
        }
      }
    }

    // Add foreground weights to the locations of the hands?
    for (unsigned int i = 0; i < arm_locs.size(); ++i)
    {
      if (arm_locs[i].x >= 0 && arm_locs[i].x < C &&
          arm_locs[i].y >= 0 && arm_locs[i].y < R)
      {
        // Add weights to the neighborhood around this point
        for (int r = max(0, arm_locs[i].y - arm_grow_radius_);
             r < min(arm_locs[i].y + arm_grow_radius_, R); ++r)
        {
          for (int c = max(0, arm_locs[i].x - arm_grow_radius_);
               c < min(arm_locs[i].x + arm_grow_radius_, C); ++c)
          {
            g->add_tweights(r*C+c, /*capacities*/ w_f_, w_n_f_);
#ifdef VISUALIZE_GRAPH_WEIGHTS
            fg_weights.at<float>(r,c) = w_f_;
            bg_weights.at<float>(r,c) = w_n_f_;
#endif // VISUALIZE_GRAPH_WEIGHTS
          }
        }
      }
    }

#ifdef VISUALIZE_GRAPH_WEIGHTS
    cv::imshow("fg_weights", fg_weights);
    cv::imshow("bg_weights", bg_weights);
#endif // VISUALIZE_GRAPH_WEIGHTS

    int flow = g->maxflow(false);

    // Convert output into image
    cv::Mat segs(R, C, CV_32FC1, cv::Scalar(0.0));
    for (int r = 0; r < R; ++r)
    {
      float* seg_row = segs.ptr<float>(r);
      for (int c = 0; c < C; ++c)
      {
        float label = (g->what_segment(r*C+c) == GraphType::SOURCE);
        seg_row[c] = label;
      }
    }
    delete g;
    return segs;
  }

  float getEdgeWeight(cv::Vec3f c0, float d0, cv::Vec3f c1, float d1)
  {
    cv::Vec3f c_d = c0-c1;
    float w_d = d0-d1;
    // float w_c = sqrt(c_d[0]*c_d[0]+c_d[1]*c_d[1]+c_d[2]*c_d[2] + w_d*w_d);
    // float w_c = sqrt(c_d[0]*c_d[0] + c_d[1]*c_d[1] + w_d*w_d);
    float w_c = sqrt(c_d[0]*c_d[0] + w_d*w_d);
    return w_c;
  }

  void setEigenRatio(double eigen_ratio)
  {
    corner_thresh_ = (eigen_ratio+1)*(eigen_ratio+1)/eigen_ratio;
  }

 protected:
  GraphType *g;
  double corner_thresh_;

 public:
  double w_f_;
  double w_b_;
  double w_n_f_;
  double w_n_b_;
  double w_w_b_;
  double w_u_f_;
  double w_u_b_;
  double magnitude_thresh_;
  int arm_grow_radius_;
};

class PR2ArmDetector
{
 public:
  PR2ArmDetector(int num_bins = 32) :
      have_hist_model_(false), theta_(0.5), num_bins_(num_bins),
      bin_size_(256/num_bins), laplace_denom_(num_bins_*num_bins_*num_bins_)
  {
    int sizes[] = {num_bins, num_bins, num_bins};
    fg_hist_.create(3, sizes, CV_32FC1);
    bg_hist_.create(3, sizes, CV_32FC1);
  }

  void setHISTColorModel(cv::Mat fg_hist, cv::Mat bg_hist, double theta)
  {
    have_hist_model_ = true;
    fg_hist_ = fg_hist;
    bg_hist_ = bg_hist;
    theta_ = theta;
  }

  float pixelArmProbability(cv::Vec3b pixel_color)
  {
    if (not have_hist_model_) return 0.0;
    cv::Vec3i bins = getBinNumbers(pixel_color);
    float p_arm = log((fg_hist_.at<float>(bins[0], bins[1], bins[2])+1) /
                      (fg_count_+laplace_denom_));
    float p_bg = log((bg_hist_.at<float>(bins[0], bins[1], bins[2])+1) /
                     (bg_count_+laplace_denom_));
    float prob = exp(p_arm - p_bg);
    return prob;
  }

  inline const cv::Vec3i getBinNumbers(cv::Vec3b pixel_color) const
  {
    const cv::Vec3i bins(pixel_color[0]/bin_size_,
                         pixel_color[1]/bin_size_,
                         pixel_color[2]/bin_size_);
    return bins;
  }

  /**
   * Class to use color model to detect pr2 arms.
   * Assumes input image is CV_8UC3 HSV image
   */
  cv::Mat imageArmProbability(cv::Mat& arm_img)
  {
    cv::Mat prob_img(arm_img.size(), CV_32FC1);
    for(int r = 0; r < arm_img.rows; ++r)
    {
      cv::Vec3b* arm_row = arm_img.ptr<cv::Vec3b>(r);
      float* prob_row = prob_img.ptr<float>(r);
      for (int c = 0; c < arm_img.cols; ++c)
      {
        float pixel_ratio = pixelArmProbability(arm_row[c]);
        if (pixel_ratio >= theta_)
        {
          prob_row[c] = 1.0;
          // ROS_INFO_STREAM("pixel_ratio_: " << pixel_ratio);
        }
        else
        {
          prob_row[c] = 0.0;
        }
      }
    }
    return prob_img;
  }

  void learnColorModels(std::string directory_path, int count,
                        std::string save_path)
  {
    ROS_INFO_STREAM("Learning color models.");
    // Empty histograms before starting
    for (int i = 0; i < num_bins_; ++i)
    {
      for (int j = 0; j < num_bins_; ++j)
      {
        for (int k = 0; k < num_bins_; ++k)
        {
          fg_hist_.at<float>(i,j,k) = 0;
          bg_hist_.at<float>(i,j,k) = 0;
        }
      }
    }
    fg_count_ = 0;
    bg_count_ = 0;

    // Add pixels form each of the things
    for (int i = 0; i < count; ++i)
    {
      std::stringstream img_name;
      img_name << directory_path << i << ".tiff";
      std::stringstream mask_name;
      mask_name << directory_path << i << "_mask.tiff";
      cv::Mat img_bgr = cv::imread(img_name.str());
      cv::Mat img;
      cv::cvtColor(img_bgr, img, CV_BGR2HSV);
      cv::Mat mask = cv::imread(mask_name.str(), 0);
      // Extract fg and bg regions and add to histogram
      for (int r = 0; r < img.rows; ++r)
      {
        cv::Vec3b* img_row = img.ptr<cv::Vec3b>(r);
        uchar* mask_row = mask.ptr<uchar>(r);
        for (int c = 0; c < img.cols; ++c)
        {
          if (mask_row[c] == 255)
          {
            cv::Vec3i bins = getBinNumbers(img_row[c]);
            fg_hist_.at<float>(bins[0], bins[1], bins[2]) += 1;
            fg_count_++;
          }
          else if (mask_row[c] == 0)
          {
            cv::Vec3i bins = getBinNumbers(img_row[c]);
            bg_hist_.at<float>(bins[0], bins[1], bins[2]) += 1;
            bg_count_++;
          }
        }
      }
    }
    saveColorModels(save_path);
  }

  void saveColorModels(std::string output_path)
  {
    std::ofstream data_out(output_path.c_str());
    // Write out header infos
    data_out << fg_count_ << std::endl;
    data_out << bg_count_ << std::endl;
    data_out << num_bins_ << std::endl;
    data_out << theta_ << std::endl;
    // Write out the histograms
    for (int i = 0; i < num_bins_; ++i)
    {
      for (int j = 0; j < num_bins_; ++j)
      {
        for (int k = 0; k < num_bins_; ++k)
        {
          data_out << fg_hist_.at<float>(i,j,k) << " ";
        }
        data_out << std::endl;
      }
    }
    data_out << std::endl;
    for (int i = 0; i < num_bins_; ++i)
    {
      for (int j = 0; j < num_bins_; ++j)
      {
        for (int k = 0; k < num_bins_; ++k)
        {
          data_out << bg_hist_.at<float>(i,j,k) << " ";
        }
        data_out << std::endl;
      }
    }
    data_out.close();
  }

  void loadColorModels(std::string input_path)
  {
    std::ifstream data_in(input_path.c_str());
    // Read in header infos
    data_in >> fg_count_;
    data_in >> bg_count_;
    data_in >> num_bins_;
    data_in >> theta_;
    laplace_denom_ = num_bins_*num_bins_*num_bins_;
    // Read in the histograms
    for (int i = 0; i < num_bins_; ++i)
    {
      for (int j = 0; j < num_bins_; ++j)
      {
        for (int k = 0; k < num_bins_; ++k)
        {
          data_in >> fg_hist_.at<float>(i,j,k);
        }
      }
    }
    for (int i = 0; i < num_bins_; ++i)
    {
      for (int j = 0; j < num_bins_; ++j)
      {
        for (int k = 0; k < num_bins_; ++k)
        {
          data_in >> bg_hist_.at<float>(i,j,k);
        }
      }
    }
    data_in.close();
    have_hist_model_ = true;
    // float prior = static_cast<float>(fg_count_) / static_cast<float>(bg_count_);
  }

  bool have_hist_model_;
  cv::Mat fg_hist_;
  cv::Mat bg_hist_;
  double theta_;
  int fg_count_;
  int bg_count_;
  int num_bins_;
  int bin_size_;
  int laplace_denom_;
};

class TabletopPushingPerceptionNode
{
 public:
  TabletopPushingPerceptionNode(ros::NodeHandle &n) :
      n_(n),
      image_sub_(n, "color_image_topic", 1),
      depth_sub_(n, "depth_image_topic", 1),
      cloud_sub_(n, "point_cloud_topic", 1),
      sync_(MySyncPolicy(15), image_sub_, depth_sub_, cloud_sub_),
      it_(n),
      track_server_(n, "seg_track_action",
                    boost::bind(&TabletopPushingPerceptionNode::trackerGoalCallback,
                                this, _1),
                    false),
      tf_(), have_depth_data_(false), tracking_(false),
      tracker_initialized_(false), roi_(0,0,640,480), min_contour_size_(30),
      tracker_count_(0)
  {
    // Get parameters from the server
    ros::NodeHandle n_private("~");
    n_private.param("crop_min_x", crop_min_x_, 0);
    n_private.param("crop_max_x", crop_max_x_, 640);
    n_private.param("crop_min_y", crop_min_y_, 0);
    n_private.param("crop_max_y", crop_max_y_, 480);
    n_private.param("display_wait_ms", display_wait_ms_, 3);
    n_private.param("min_workspace_x", min_workspace_x_, 0.0);
    n_private.param("min_workspace_y", min_workspace_y_, 0.0);
    n_private.param("min_workspace_z", min_workspace_z_, 0.0);
    n_private.param("max_workspace_x", max_workspace_x_, 0.0);
    n_private.param("max_workspace_y", max_workspace_y_, 0.0);
    n_private.param("max_workspace_z", max_workspace_z_, 0.0);
    std::string default_workspace_frame = "/torso_lift_link";
    n_private.param("workspace_frame", workspace_frame_,
                    default_workspace_frame);
    n_private.param("min_table_z", min_table_z_, -0.5);
    n_private.param("max_table_z", max_table_z_, 1.5);
    n_private.param("autostart_tracking", tracking_, false);
    n_private.param("min_contour_size", min_contour_size_, 30);
    n_private.param("num_downsamples", num_downsamples_, 2);
    std::string cam_info_topic_def = "/kinect_head/rgb/camera_info";
    n_private.param("cam_info_topic", cam_info_topic_,
                    cam_info_topic_def);

    base_output_path_ = "/home/thermans/sandbox/cut_out/";
    // Graphcut weights
    n_private.param("mgc_w_f", mgc_.w_f_, 3.0);
    n_private.param("mgc_w_b", mgc_.w_b_, 2.0);
    n_private.param("mgc_w_n_f", mgc_.w_n_f_, 0.01);
    n_private.param("mgc_w_n_b", mgc_.w_n_b_, 0.01);
    n_private.param("mgc_w_w_b", mgc_.w_w_b_, 5.0);
    n_private.param("mgc_w_u_f", mgc_.w_u_f_, 0.1);
    n_private.param("mgc_w_u_b", mgc_.w_u_b_, 0.1);
    n_private.param("mgc_arm_grow_radius", mgc_.arm_grow_radius_, 2);

    // Lucas Kanade params
    double eigen_ratio = 0.5;
    n_private.param("mgc_eigen_ratio", eigen_ratio, 0.5);
    mgc_.setEigenRatio(eigen_ratio);
    n_private.param("mgc_magnitude_thresh", mgc_.magnitude_thresh_, 0.1);
    int win_size = 5;
    n_private.param("lk_win_size", win_size, 5);
    lkflow_.setWinSize(win_size);
    int num_levels = 4;
    n_private.param("lk_num_levels", num_levels, 4);
    lkflow_.setNumLevels(num_levels);

    // Arm detections stuff
    // std::string package_path = ros::package::getPath("tabletop_pushing");
    // std::stringstream arm_data_default_path;
    // arm_data_default_path << package_path << "/cfg/pr2_arm_model0.txt";
    // n_private.param("arm_detection_model_path", arm_data_path_,
    //                 arm_data_default_path.str());
    // arm_detect_.loadColorModels(arm_data_path_);
    // n_private.param("arm_detection_theta", arm_detect_.theta_, 0.5);

    // Setup ros node connections
    sync_.registerCallback(&TabletopPushingPerceptionNode::sensorCallback,
                           this);
    push_pose_server_ = n_.advertiseService(
        "get_push_pose", &TabletopPushingPerceptionNode::getPushPose, this);
    table_location_server_ = n_.advertiseService(
        "get_table_location", &TabletopPushingPerceptionNode::getTableLocation,
        this);
    motion_mask_pub_ = it_.advertise("motion_mask", 15);
    motion_img_pub_ = it_.advertise("motion_img", 15);
    roi_.x = crop_min_x_;
    roi_.y = crop_min_y_;
    roi_.width = crop_max_x_ - crop_min_x_;
    roi_.height = crop_max_y_ - crop_min_y_;
    track_server_.start();
  }

  void sensorCallback(const sensor_msgs::ImageConstPtr& img_msg,
                      const sensor_msgs::ImageConstPtr& depth_msg,
                      const sensor_msgs::PointCloud2ConstPtr& cloud_msg)
  {
    // Convert images to OpenCV format
    cv::Mat color_frame(bridge_.imgMsgToCv(img_msg));
    cv::Mat depth_frame(bridge_.imgMsgToCv(depth_msg));

    // Swap kinect color channel order
    cv::cvtColor(color_frame, color_frame, CV_RGB2BGR);

    // Transform point cloud into the correct frame and convert to PCL struct
    XYZPointCloud cloud;
    pcl::fromROSMsg(*cloud_msg, cloud);
    pcl_ros::transformPointCloud(workspace_frame_, cloud, cloud, tf_);

    // Convert nans to zeros
    for (int r = 0; r < depth_frame.rows; ++r)
    {
      float* depth_row = depth_frame.ptr<float>(r);
      for (int c = 0; c < depth_frame.cols; ++c)
      {
        float cur_d = depth_row[c];
        if (isnan(cur_d))
        {
          depth_row[c] = 0.0;
        }
      }
    }

    cv::Mat workspace_mask(color_frame.rows, color_frame.cols, CV_8UC1,
                           cv::Scalar(255));
    // Black out pixels in color and depth images outside of workspace
    // As well as outside of the crop window
    for (int r = 0; r < color_frame.rows; ++r)
    {
      uchar* workspace_row = workspace_mask.ptr<uchar>(r);
      for (int c = 0; c < color_frame.cols; ++c)
      {
        // NOTE: Cloud is accessed by at(column, row)
        pcl::PointXYZ cur_pt = cloud.at(c, r);
        if (cur_pt.x < min_workspace_x_ || cur_pt.x > max_workspace_x_ ||
            cur_pt.y < min_workspace_y_ || cur_pt.y > max_workspace_y_ ||
            cur_pt.z < min_workspace_z_ || cur_pt.z > max_workspace_z_ ||
            r < crop_min_y_ || c < crop_min_x_ || r > crop_max_y_ ||
            c > crop_max_x_ )
        {
          workspace_row[c] = 0;
        }
      }
    }

    // Downsample everything first
    cv::Mat color_frame_down = downSample(color_frame, num_downsamples_);
    cv::Mat depth_frame_down = downSample(depth_frame, num_downsamples_);
    cv::Mat workspace_mask_down = downSample(workspace_mask, num_downsamples_);

    // Save internally for use in the service callback
    prev_color_frame_ = cur_color_frame_.clone();
    prev_depth_frame_ = cur_depth_frame_.clone();
    prev_workspace_mask_ = cur_workspace_mask_.clone();
    prev_camera_header_ = cur_camera_header_;

    // Update the current versions
    cur_color_frame_ = color_frame_down.clone();
    cur_depth_frame_ = depth_frame_down.clone();
    cur_workspace_mask_ = workspace_mask_down.clone();
    cur_point_cloud_ = cloud;
    have_depth_data_ = true;
    cur_camera_header_ = img_msg->header;

    // Started via actionlib call
    if (tracking_)
    {
      cv::Mat motion_mask = segmentMovingStuff(cur_color_frame_,
                                               cur_depth_frame_,
                                               prev_color_frame_,
                                               prev_depth_frame_,
                                               cur_point_cloud_);

      // TODO: Process the moving stuff to separate arm from other stuff
      prev_motion_mask_ = motion_mask.clone();
    }
  }

  bool getTableLocation(LocateTable::Request& req, LocateTable::Response& res)
  {
    if ( have_depth_data_ )
    {
      res.table_centroid = getTablePlane(cur_point_cloud_);
      if (res.table_centroid.pose.position.x == 0.0 &&
          res.table_centroid.pose.position.y == 0.0 &&
          res.table_centroid.pose.position.z == 0.0)
      {
        ROS_ERROR_STREAM("No plane found, leaving");
        return false;
      }

    }
    else
    {
      ROS_ERROR_STREAM("Calling getTableLocation prior to receiving sensor data.");
      return false;
    }
    return true;
  }

  bool getPushPose(PushPose::Request& req, PushPose::Response& res)
  {
    if ( have_depth_data_ )
    {
      res = findPushPose(cur_color_frame_, cur_depth_frame_, cur_point_cloud_);
    }
    else
    {
      ROS_ERROR_STREAM("Calling getPushPose prior to receiving sensor data.");
      return false;
    }
    return true;
  }

  PushPose::Response findPushPose(cv::Mat visual_frame,
                                  cv::Mat depth_frame,
                                  XYZPointCloud& cloud)
  {

    // TODO: Choose a patch based on some simple criterian
    // TODO: Estimate the surface of the patch from the depth image
    // TODO: Extract the push pose as point in the center of that surface
    // TODO: Transform to be in the workspace_frame
    PushPose::Response res;
    PoseStamped p;
    res.push_pose = p;
    res.invalid_push_pose = false;
    return res;
  }

  //
  // Region tracking methods
  //

  void trackerGoalCallback(const tabletop_pushing::SegTrackGoalConstPtr &goal)
  {
    ROS_INFO_STREAM("Tracker callback.");
    if (goal->start)
    {
      ROS_INFO_STREAM("Starting tracker.");
      startTracker();
      tabletop_pushing::SegTrackResult result;
      track_server_.setSucceeded(result);
    }
    else
    {
      ROS_INFO_STREAM("Stopping tracker.");
      stopTracker();
      track_server_.setPreempted();
    }
  }

  void startTracker()
  {
    tracker_initialized_ = false;
    tracking_ = true;

  }
  void stopTracker()
  {
    tracker_initialized_ = false;
    tracking_ = false;
  }

  //
  // Core method for calculation
  //
  cv::Mat segmentMovingStuff(cv::Mat& color_frame, cv::Mat& depth_frame,
                             cv::Mat& prev_color_frame, cv::Mat& prev_depth_frame,
                             XYZPointCloud& cloud)
  {
    if (!tracker_initialized_)
    {
      ROS_INFO_STREAM("Initializing tracker.");
      table_centroid_ = getTablePlane(cloud);
      if (table_centroid_.pose.position.x == 0.0 &&
          table_centroid_.pose.position.y == 0.0 &&
          table_centroid_.pose.position.z == 0.0)
      {
        ROS_ERROR_STREAM("No plane found!");
      }
      cam_info_ = *ros::topic::waitForMessage<sensor_msgs::CameraInfo>(
          cam_info_topic_, n_, ros::Duration(5.0));

      tracker_initialized_ = true;
      tracker_count_ = 0;
      cv::Mat empty_motion(color_frame.rows, color_frame.cols, CV_8UC1,
                           cv::Scalar(0));
      return empty_motion;
    }

    cv::Mat color_frame_hsv(color_frame.size(), color_frame.type());
    cv::cvtColor(color_frame, color_frame_hsv, CV_BGR2HSV);

    std::vector<cv::Mat> flow_outs = lkflow_(color_frame, prev_color_frame);

    std::vector<cv::Point> arms = projectArmPoseIntoImage(cur_camera_header_);

    // TODO: Use this structure after segmenting moving form non-moving to
    // segment arm from not-arm
    // getColorModels(color_frame_hsv, arms);

    cv::Mat color_frame_f(color_frame_hsv.size(), CV_32FC3);
    color_frame_hsv.convertTo(color_frame_f, CV_32FC3, 1.0/255, 0);
    cv::Mat cut = mgc_(color_frame_f, depth_frame, flow_outs[0], flow_outs[1],
                       flow_outs[2], cur_workspace_mask_, arms);


    cv::Mat cleaned_cut(cut.size(), CV_8UC1);
    cut.convertTo(cleaned_cut, CV_8UC1, 255, 0);
#ifdef REMOVE_SMALL_BLOBS
    // Remove blobs smaller than min_contour_size_
    std::vector<std::vector<cv::Point> > contours;
    cv::findContours(cleaned_cut, contours, CV_RETR_EXTERNAL,
                     CV_CHAIN_APPROX_NONE);
    for (unsigned int i = 0; i < contours.size(); ++i)
    {
      cv::Moments m;
      cv::Mat pt_mat = cv::Mat(contours[i]);
      m = cv::moments(pt_mat);
      if (m.m00 < min_contour_size_)
      {
        for (unsigned int j = 0; j < contours[i].size(); ++j)
        {
          cleaned_cut.at<uchar>(contours[i][j].y, contours[i][j].x) = 0;
        }
      }
    }
#endif // REMOVE_SMALL_BLOBS

    // Publish the moving region stuff
    cv_bridge::CvImage motion_mask_msg;
    motion_mask_msg.image = cleaned_cut;
    motion_mask_msg.header = cur_camera_header_;
    motion_mask_msg.encoding = sensor_msgs::image_encodings::TYPE_8UC1;
    motion_mask_pub_.publish(motion_mask_msg.toImageMsg());

    // Also publish color version
    cv::Mat moving_regions_img;
    color_frame.copyTo(moving_regions_img, cleaned_cut);
    cv_bridge::CvImage motion_img_msg;
    cv::Mat motion_img_send(cut.size(), CV_8UC3);
    moving_regions_img.convertTo(motion_img_send, CV_8UC3, 1.0, 0);
    motion_img_msg.image = motion_img_send;
    motion_img_msg.header = cur_camera_header_;
    motion_img_msg.encoding = sensor_msgs::image_encodings::TYPE_8UC3;
    motion_img_pub_.publish(motion_img_msg.toImageMsg());

#ifdef WRITE_INPUT_TO_DISK
    std::stringstream input_out_name;
    input_out_name << base_output_path_ << "input" << tracker_count_ << ".tiff";
    cv::imwrite(input_out_name.str(), color_frame);
#endif // WRITE_INPUT_TO_DISK

#ifdef WRITE_FLOWS_TO_DISK
    cv::Mat flow_thresh_disp_img(color_frame.size(), CV_8UC3);
    flow_thresh_disp_img = color_frame.clone();

    float corner_thresh = (eigen_ratio_+1)*(eigen_ratio_+1)/eigen_ratio_;
    for (int r = 0; r < flow_thresh_disp_img.rows; ++r)
    {
      for (int c = 0; c < flow_thresh_disp_img.cols; ++c)
      {
        float u = flow_outs[0].at<float>(r,c);
        float v = flow_outs[1].at<float>(r,c);
        if (std::sqrt(u*u+v*v) > mgc_.magnitude_thresh_ &&
            flow_outs[2].at<float>(r,c) < corner_thresh)
        {
          cv::line(flow_thresh_disp_img, cv::Point(c,r),
                   cv::Point(c-u, r-v), cv::Scalar(0,255,0));
        }
      }
    }

    std::stringstream flow_out_name;
    flow_out_name << base_output_path_ << "flow" << tracker_count_ << ".tiff";
    cv::imwrite(flow_out_name.str(), flow_thresh_disp_img);
#endif // WRITE_FLOWS_TO_DISK

#ifdef WRITE_CUTS_TO_DISK
    std::stringstream cut_out_name;
    cut_out_name << base_output_path_ << "cut" << tracker_count_ << ".tiff";
    cv::imwrite(cut_out_name.str(), moving_regions_img);
#endif // WRITE_CUTS_TO_DISK

#ifdef WRITE_ARMS_TO_DISK
    std::stringstream arm_out_name;
    arm_out_name << base_output_path_ << "arm" << tracker_count_ << ".tiff";
    cv::Mat arm_write_img(color_frame.size(), CV_8UC3);
    arm_write_img = color_frame.clone();
    for (unsigned int i = 0; i < arms.size(); ++i)
    {
      cv::circle(arm_write_img, arms[i], 2, cv::Scalar(0,255,0));
    }
    cv::imwrite(arm_out_name.str(), arm_write_img);
#endif // WRITE_ARMS_TO_DISK

#ifdef DISPLAY_INPUT_COLOR
    std::vector<cv::Mat> hsv;
    cv::split(color_frame_hsv, hsv);
    // cv::imshow("hue", hsv[0]);
    // cv::imshow("saturation", hsv[1]);
    // cv::imshow("intensity", hsv[2]);
    cv::imshow("input_color", color_frame);
#endif // DISPLAY_INPUT_COLOR
#ifdef DISPLAY_ARM_CIRCLES
    cv::Mat arms_img(color_frame.size(), CV_8UC3);
    arms_img = color_frame.clone();
    for (unsigned int i = 0; i < arms.size(); ++i)
    {
      cv::circle(arms_img, arms[i], 2, cv::Scalar(0,255,0));
    }
    cv::imshow("arms", arms_img);
#endif
#ifdef DISPLAY_INPUT_DEPTH
    cv::imshow("input_depth", depth_frame);
#endif // DISPLAY_INPUT_DEPTH
#ifdef DISPLAY_WORKSPACE_MASK
    cv::imshow("workspace_mask", cur_workspace_mask_);
#endif // DISPLAY_WORKSPACE_MASK
#ifdef DISPLAY_ARM_PROBS
    cv::imshow("arm_probs", arm_probs);
#endif // DISPLAY_ARM_PROBS
#ifdef DISPLAY_OPTICAL_FLOW
    displayOpticalFlow(color_frame, flow_outs[0], flow_outs[1],
                       mgc_.magnitude_thresh_);
#endif // DISPLAY_OPTICAL_FLOW

#ifdef DISPLAY_GRAPHCUT
    // cv::imshow("Cut", cut);
    // cv::imshow("Cleaned Cut", cleaned_cut);
    cv::imshow("moving_regions", moving_regions_img);
#endif // DISPLAY_GRAPHCUT

#if defined DISPLAY_INPUT_COLOR || defined DISPLAY_INPUT_DEPTH || defined DISPLAY_OPTICAL_FLOW || defined DISPLAY_GRAPHCUT || defined DISPLAY_GRAPHCUT || defined DISPLAY_WORKSPACE_MASK || defined DISPLAY_MOTION_PROBS || defined DISPLAY_MEANS || defined DISPLAY_VARS || defined DISPLAY_INTERMEDIATE_PROBS || defined DISPLAY_OPT_FLOW_INTERNALS || defined DISPLAY_OPT_FLOW_II_INTERNALS || defined DISPLAY_GRAPHCUT || defined VISUALIZE_GRAPH_WEIGHTS
    cv::waitKey(display_wait_ms_);
#endif // Any display defined
    ++tracker_count_;
    return cleaned_cut;
  }

  //
  // Arm detection methods
  //
  cv::Point projectPointIntoImage(PointStamped cur_point,
                                  std::string target_frame)
  {
    cv::Point img_loc;
    try
    {
      // Transform point into the camera frame
      PointStamped image_frame_loc_m;
      tf_.transformPoint(target_frame, cur_point, image_frame_loc_m);

      // Project point onto the image
      img_loc.x = static_cast<int>((cam_info_.K[0]*image_frame_loc_m.point.x +
                                    cam_info_.K[2]*image_frame_loc_m.point.z) /
                                   image_frame_loc_m.point.z);
      img_loc.y = static_cast<int>((cam_info_.K[4]*image_frame_loc_m.point.y +
                                    cam_info_.K[5]*image_frame_loc_m.point.z) /
                                   image_frame_loc_m.point.z);

      // Downsample poses if the image is downsampled
      for (int i = 0; i < num_downsamples_; ++i)
      {
        img_loc.x /= 2;
        img_loc.y /= 2;
      }
    }
    catch (tf::TransformException e)
    {
      ROS_ERROR_STREAM(e.what());
    }
    return img_loc;
  }

  void getLineValues(cv::Point p1, cv::Point p2, std::vector<cv::Point>& line)
  {
    bool steep = (abs(p1.y - p2.y) > abs(p1.x - p2.x));
    if (steep)
    {
      // Swap x and y
      cv::Point tmp(p1.y, p1.x);
      p1.x = tmp.x;
      p1.y = tmp.y;
      tmp.y = p2.x;
      tmp.x = p2.y;
      p2.x = tmp.x;
      p2.y = tmp.y;
    }
    if (p1.x > p2.x)
    {
      // Swap p1 and p2
      cv::Point tmp(p1.x, p1.y);
      p1.x = p2.x;
      p1.y = p2.y;
      p2.x = tmp.x;
      p2.y = tmp.y;
    }
    int dx = p2.x - p1.x;
    int dy = abs(p2.y - p1.y);
    int error = dx / 2;
    int ystep = 0;
    if (p1.y < p2.y)
    {
      ystep = 1;
    }
    else if (p1.y > p2.y)
    {
      ystep = -1;
    }
    for(int x = p1.x, y = p1.y; x <= p2.x; ++x)
    {
      if (steep)
      {
        cv::Point p_new(y,x);
        line.push_back(p_new);
      }
      else
      {
        cv::Point p_new(x,y);
        line.push_back(p_new);
      }
      error -= dy;
      if (error < 0)
      {
        y += ystep;
        error += dx;
      }
    }
  }

  std::vector<cv::Point> projectArmPoseIntoImage(std_msgs::Header img_header)
  {
    // Project all arm joints into image
    std::vector<cv::Point> arm_locs;
    // Left arm
    cv::Point l0 = projectJointOriginIntoImage(img_header, "l_gripper_tool_frame");
    cv::Point l1 = projectJointOriginIntoImage(img_header, "l_wrist_flex_link");
    cv::Point l2 = projectJointOriginIntoImage(img_header, "l_forearm_roll_link");
    // Right arm
    cv::Point r0 = projectJointOriginIntoImage(img_header, "r_gripper_tool_frame");
    cv::Point r1 = projectJointOriginIntoImage(img_header, "r_wrist_flex_link");
    cv::Point r2 = projectJointOriginIntoImage(img_header, "r_forearm_roll_link");

    getLineValues(l0, l1, arm_locs);
    getLineValues(l1, l2, arm_locs);
    getLineValues(r0, r1, arm_locs);
    getLineValues(r1, r2, arm_locs);

    return arm_locs;
  }
  cv::Point projectJointOriginIntoImage(std_msgs::Header img_header,
                                        std::string joint_name)
  {
    PointStamped joint_origin;
    joint_origin.header.frame_id = joint_name;
    joint_origin.header.stamp = img_header.stamp;
    joint_origin.point.x = 0.0;
    joint_origin.point.y = 0.0;
    joint_origin.point.z = 0.0;
    return projectPointIntoImage(joint_origin, img_header.frame_id);
  }

  //
  // Helper Methods
  //
  cv::Mat downSample(cv::Mat data_in, int scales)
  {
    cv::Mat out = data_in.clone();
    for (int i = 0; i < scales; ++i)
    {
      cv::pyrDown(data_in, out);
      data_in = out;
    }
    return out;
  }

  PoseStamped getTablePlane(XYZPointCloud& cloud)
  {
    // Filter Cloud to not look for table planes on the ground
    pcl::PointCloud<pcl::PointXYZ> cloud_z_filtered;
    pcl::PassThrough<pcl::PointXYZ> z_pass;
    z_pass.setInputCloud(
        boost::make_shared<pcl::PointCloud<pcl::PointXYZ> >(cloud));
    z_pass.setFilterFieldName ("z");
    z_pass.setFilterLimits(min_table_z_, max_table_z_);
    z_pass.filter(cloud_z_filtered);

    // Segment the tabletop from the points using RANSAC plane fitting
    pcl::ModelCoefficients coefficients;
    pcl::PointIndices plane_inliers;
    // Create the segmentation object
    pcl::SACSegmentation<pcl::PointXYZ> plane_seg;
    plane_seg.setOptimizeCoefficients (true);
    plane_seg.setModelType (pcl::SACMODEL_PLANE);
    plane_seg.setMethodType (pcl::SAC_RANSAC);
    plane_seg.setDistanceThreshold (0.01);
    plane_seg.setInputCloud (
        boost::make_shared<pcl::PointCloud<pcl::PointXYZ> >(cloud_z_filtered));
    plane_seg.segment(plane_inliers, coefficients);

    // Check size of plane_inliers
    if (plane_inliers.indices.size() < 1)
    {
      ROS_WARN_STREAM("No points found by RANSAC plane fitting");
      PoseStamped p;
      p.pose.position.x = 0.0;
      p.pose.position.y = 0.0;
      p.pose.position.z = 0.0;
      p.header = cloud.header;
      return p;
    }

    // Extract the plane members into their own point cloud
    pcl::PointCloud<pcl::PointXYZ> plane_cloud;
    pcl::copyPointCloud(cloud_z_filtered, plane_inliers, plane_cloud);

    // Return plane centroid
    Eigen::Vector4f xyz_centroid;
    pcl::compute3DCentroid(plane_cloud, xyz_centroid);
    PoseStamped p;
    p.pose.position.x = xyz_centroid[0];
    p.pose.position.y = xyz_centroid[1];
    p.pose.position.z = xyz_centroid[2];
    p.header = cloud.header;
    ROS_INFO_STREAM("Table centroid is: ("
                    << p.pose.position.x << ", "
                    << p.pose.position.y << ", "
                    << p.pose.position.z << ")");
    // TODO: Get extent as well
    pcl::PointCloud<pcl::PointXYZ> cloud_hull;
    pcl::ConvexHull<pcl::PointXYZ> hull;
    hull.setInputCloud(boost::make_shared<
                       pcl::PointCloud<pcl::PointXYZ> >(plane_cloud));
    // hull.setAlpha(0.1);
    hull.reconstruct(cloud_hull);
    ROS_INFO_STREAM("Convex hull has: " << cloud_hull.points.size()
                    << " points");
    return p;
  }

  /**
   * Executive control function for launching the node.
   */
  void spin()
  {
    while(n_.ok())
    {
      ros::spinOnce();
    }
  }

 protected:
  ros::NodeHandle n_;
  message_filters::Subscriber<sensor_msgs::Image> image_sub_;
  message_filters::Subscriber<sensor_msgs::Image> depth_sub_;
  message_filters::Subscriber<sensor_msgs::PointCloud2> cloud_sub_;
  message_filters::Synchronizer<MySyncPolicy> sync_;
  image_transport::ImageTransport it_;
  sensor_msgs::CameraInfo cam_info_;
  image_transport::Publisher motion_img_pub_;
  image_transport::Publisher motion_mask_pub_;
  actionlib::SimpleActionServer<tabletop_pushing::SegTrackAction> track_server_;
  sensor_msgs::CvBridge bridge_;
  tf::TransformListener tf_;
  ros::ServiceServer push_pose_server_;
  ros::ServiceServer table_location_server_;
  cv::Mat cur_color_frame_;
  cv::Mat cur_depth_frame_;
  cv::Mat cur_workspace_mask_;
  cv::Mat prev_color_frame_;
  cv::Mat prev_depth_frame_;
  cv::Mat prev_workspace_mask_;
  cv::Mat prev_motion_mask_;
  std_msgs::Header cur_camera_header_;
  std_msgs::Header prev_camera_header_;
  XYZPointCloud cur_point_cloud_;
  LKFlowReliable lkflow_;
  MotionGraphcut mgc_;
  bool have_depth_data_;
  int crop_min_x_;
  int crop_max_x_;
  int crop_min_y_;
  int crop_max_y_;
  int display_wait_ms_;
  double min_workspace_x_;
  double max_workspace_x_;
  double min_workspace_y_;
  double max_workspace_y_;
  double min_workspace_z_;
  double max_workspace_z_;
  double min_table_z_;
  double max_table_z_;
  double eigen_ratio_;
  int num_downsamples_;
  std::string workspace_frame_;
  PoseStamped table_centroid_;
  bool tracking_;
  bool tracker_initialized_;
  cv::Rect roi_;
  // PR2ArmDetector arm_detect_;
  std::string arm_data_path_;
  std::string cam_info_topic_;
  int min_contour_size_;
  int tracker_count_;
  std::string base_output_path_;
};

int main(int argc, char ** argv)
{
  ros::init(argc, argv, "tabletop_perception_node");
  ros::NodeHandle n;
  TabletopPushingPerceptionNode perception_node(n);
  perception_node.spin();

  return 0;
}

