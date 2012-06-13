/*********************************************************************
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.

 \file    policy_improvement.cpp

 \author  Ludovic Righetti, Peter Pastor, Mrinal Kalakrishnan
 \date    May 26, 2010

 **********************************************************************/

// system includes
#include <time.h>
#include <cfloat>
#include <ros/assert.h>

#include <Eigen/Core>
#include <Eigen/LU>

// local includes
#include <ros/ros.h>
#include <stomp/policy_improvement.h>
#include <usc_utilities/assert.h>
#include <algorithm>

using namespace Eigen;

namespace stomp
{

PolicyImprovement::PolicyImprovement():
    initialized_(false)
{
}

PolicyImprovement::~PolicyImprovement()
{
}

bool PolicyImprovement::initialize(const int num_time_steps,
                                   const int min_rollouts,
                                   const int max_rollouts,
                                   const int num_rollouts_per_iteration,
                                   boost::shared_ptr<CovariantMovementPrimitive> policy,
                                   bool use_cumulative_costs)
{
  num_time_steps_ = num_time_steps;
  use_cumulative_costs_ = use_cumulative_costs;
  policy_ = policy;

  ROS_VERIFY(policy_->setNumTimeSteps(num_time_steps_));
  ROS_VERIFY(policy_->getControlCosts(control_costs_));
  ROS_VERIFY(policy_->getNumDimensions(num_dimensions_));
  ROS_VERIFY(policy_->getNumParameters(num_parameters_));
  ROS_VERIFY(policy_->getBasisFunctions(basis_functions_));
  ROS_VERIFY(policy_->getParameters(parameters_));

  // invert the control costs, initialize noise generators:
  inv_control_costs_.clear();
  noise_generators_.clear();
  for (int d=0; d<num_dimensions_; ++d)
  {
    inv_control_costs_.push_back(control_costs_[d].fullPivLu().inverse());
    MultivariateGaussian mvg(VectorXd::Zero(num_parameters_[d]), inv_control_costs_[d]);
    noise_generators_.push_back(mvg);
  }

  ROS_VERIFY(setNumRollouts(min_rollouts, max_rollouts, num_rollouts_per_iteration));
  ROS_VERIFY(preAllocateTempVariables());
  ROS_VERIFY(preComputeProjectionMatrices());

  return (initialized_ = true);
}

bool PolicyImprovement::setNumRollouts(const int min_rollouts,
                                       const int max_rollouts,
                                       const int num_rollouts_per_iteration)
{
  min_rollouts_ = min_rollouts;
  max_rollouts_ = max_rollouts;
  num_rollouts_per_iteration_ = num_rollouts_per_iteration;
  num_rollouts_ = 0;
  num_rollouts_gen_ = 0;

  // preallocate memory for a single rollout:
  Rollout rollout;

  rollout.parameters_.clear();
  rollout.parameters_noise_.clear();
  rollout.noise_.clear();
  rollout.noise_projected_.clear();
  rollout.parameters_noise_projected_.clear();
  rollout.control_costs_.clear();
  rollout.total_costs_.clear();
  rollout.cumulative_costs_.clear();
  rollout.probabilities_.clear();
  for (int d=0; d<num_dimensions_; ++d)
  {
      rollout.parameters_.push_back(VectorXd::Zero(num_parameters_[d]));
      rollout.parameters_noise_.push_back(VectorXd::Zero(num_parameters_[d]));
      rollout.parameters_noise_projected_.push_back(VectorXd::Zero(num_parameters_[d]));
      rollout.noise_.push_back(VectorXd::Zero(num_parameters_[d]));
      rollout.noise_projected_.push_back(VectorXd::Zero(num_parameters_[d]));
      rollout.control_costs_.push_back(VectorXd::Zero(num_time_steps_));
      rollout.total_costs_.push_back(VectorXd::Zero(num_time_steps_));
      rollout.cumulative_costs_.push_back(VectorXd::Zero(num_time_steps_));
      rollout.probabilities_.push_back(VectorXd::Zero(num_time_steps_));
  }
  rollout.state_costs_ = VectorXd::Zero(num_time_steps_);

  // duplicate this rollout:
  for (int r=0; r<max_rollouts_; ++r)
  {
    rollouts_.push_back(rollout);
    reused_rollouts_.push_back(rollout);
  }


//    for (int r=0; r<num_reused_rollouts; ++r)
//        reused_rollouts_.push_back(rollout);
//
//    for (int r=0; r<num_extra_rollouts; ++r)
//        extra_rollouts_.push_back(rollout);

//    rollouts_reused_ = false;
//    rollouts_reused_next_ = false;
//    extra_rollouts_added_ = false;
  rollout_cost_sorter_.reserve(max_rollouts_);

  return true;
}

double Rollout::getCost()
{
  double cost = 0.0;
  double state_cost = state_costs_.sum();
  int num_dim = control_costs_.size();
  for (int d=0; d<num_dim; ++d)
    cost += control_costs_[d].sum();
  //printf("State cost = %f, control cost = %f\n", state_cost, cost);
  cost += state_cost;
  return cost;
}

bool PolicyImprovement::generateRollouts(const std::vector<double>& noise_stddev)
{
    ROS_ASSERT(initialized_);
    ROS_ASSERT(static_cast<int>(noise_stddev.size()) == num_dimensions_);

    // save the latest policy parameters:
    ROS_VERIFY(copyParametersFromPolicy());

    // decide how many new rollouts we will generate and discard
    int num_rollouts_discard = 0;
    int num_rollouts_reused = num_rollouts_;
    int prev_num_rollouts = num_rollouts_;
    num_rollouts_gen_ = num_rollouts_per_iteration_;
    if (num_rollouts_ + num_rollouts_gen_ < min_rollouts_)
    {
      num_rollouts_gen_ = min_rollouts_ - num_rollouts_;
      num_rollouts_discard = 0;
      num_rollouts_reused = num_rollouts_;
    }
    if (num_rollouts_ + num_rollouts_gen_ > max_rollouts_)
    {
      num_rollouts_discard = num_rollouts_ + num_rollouts_gen_ - max_rollouts_;
      num_rollouts_reused = num_rollouts_ - num_rollouts_discard;
    }
    num_rollouts_ = num_rollouts_reused + num_rollouts_gen_;

//    ROS_INFO("num_rollouts = %d", num_rollouts_);
//    ROS_INFO("num_rollouts_gen = %d", num_rollouts_gen_);
//    ROS_INFO("num_rollouts_discard = %d", num_rollouts_discard);
//    ROS_INFO("num_rollouts_reused = %d", num_rollouts_reused);

    if (num_rollouts_reused > 0)
    {
      // figure out which rollouts to reuse
      rollout_cost_sorter_.clear();
      for (int r=0; r<prev_num_rollouts; ++r)
      {
          double cost = rollouts_[r].getCost();
          rollout_cost_sorter_.push_back(std::make_pair(cost,r));
      }
      std::sort(rollout_cost_sorter_.begin(), rollout_cost_sorter_.end());

      // use the best ones: (copy them into reused_rollouts)
      for (int r=0; r<num_rollouts_reused; ++r)
      {
          int reuse_index = rollout_cost_sorter_[r].second;
          reused_rollouts_[r] = rollouts_[reuse_index];
          //double reuse_cost = rollout_cost_sorter_[r].first;
          //ROS_INFO("Reuse %d, cost = %lf", r, reuse_cost);
      }

      // copy them back from reused_rollouts_ into rollouts_
      for (int r=0; r<num_rollouts_reused; ++r)
      {
          rollouts_[num_rollouts_gen_+r] = reused_rollouts_[r];

          // update the noise based on the new parameters:
          rollouts_[num_rollouts_gen_+r].parameters_ = parameters_;
          for (int d=0; d<num_dimensions_; ++d)
          {
            // parameters_noise_projected remains the same, compute everything else from it.
            rollouts_[num_rollouts_gen_+r].noise_projected_[d] = rollouts_[num_rollouts_gen_+r].parameters_noise_projected_[d] - parameters_[d];
            rollouts_[num_rollouts_gen_+r].noise_[d] = inv_projection_matrix_[d] * rollouts_[num_rollouts_gen_+r].noise_projected_[d];
            rollouts_[num_rollouts_gen_+r].parameters_noise_[d] = parameters_[d] + rollouts_[num_rollouts_gen_+r].noise_[d];
          }
      }
    }

    // generate new rollouts
    for (int d=0; d<num_dimensions_; ++d)
    {
        for (int r=0; r<num_rollouts_gen_; ++r)
        {
            noise_generators_[d].sample(tmp_noise_[d]);
            rollouts_[r].noise_[d] = noise_stddev[d]*tmp_noise_[d];
            rollouts_[r].parameters_[d] = parameters_[d];// + rollouts_[r].noise_[d];
            rollouts_[r].parameters_noise_[d] = parameters_[d] + rollouts_[r].noise_[d];
        }
    }

    return true;
}

bool PolicyImprovement::getRollouts(std::vector<std::vector<Eigen::VectorXd> >& rollouts, const std::vector<double>& noise_variance)
{
    if (!generateRollouts(noise_variance))
    {
        ROS_ERROR("Failed to generate rollouts.");
        return false;
    }

    rollouts.clear();
    for (int r=0; r<num_rollouts_gen_; ++r)
    {
        rollouts.push_back(rollouts_[r].parameters_noise_);
    }

    return true;
}

bool PolicyImprovement::getProjectedRollouts(std::vector<std::vector<Eigen::VectorXd> >& rollouts)
{
  rollouts.clear();
  for (int r=0; r<num_rollouts_gen_; ++r)
  {
      rollouts.push_back(rollouts_[r].parameters_noise_projected_);
  }

  return true;
}


bool PolicyImprovement::setRollouts(const std::vector<std::vector<Eigen::VectorXd> >& rollouts)
{
  ROS_ASSERT((int)rollouts.size() == num_rollouts_gen_);
  for (int r=0; r<num_rollouts_gen_; ++r)
  {
    rollouts_[r].parameters_noise_ = rollouts[r];
    computeNoise(rollouts_[r]);
  }
  return true;
}

void PolicyImprovement::clearReusedRollouts()
{
  num_rollouts_ = 0;
}

bool PolicyImprovement::setRolloutCosts(const Eigen::MatrixXd& costs, const double control_cost_weight, std::vector<double>& rollout_costs_total)
{
    ROS_ASSERT(initialized_);

    control_cost_weight_ = control_cost_weight;
    computeRolloutControlCosts();

    for (int r=0; r<num_rollouts_gen_; ++r)
    {
        rollouts_[r].state_costs_ = costs.row(r).transpose();
    }

    // set the total costs
    rollout_costs_total.resize(num_rollouts_);
    for (int r=0; r<num_rollouts_; ++r)
    {
        rollout_costs_total[r] = rollouts_[r].getCost();
    }

    //debug
    for (int r=0; r<num_rollouts_gen_; ++r)
    {
      ROS_INFO("Noisy %d, cost = %lf", r, rollouts_[r].getCost());
    }
    return true;
}

bool PolicyImprovement::computeProjectedNoise()
{
    for (int r=0; r<num_rollouts_; ++r)
    {
        computeProjectedNoise(rollouts_[r]);
    }
    return true;
}

bool PolicyImprovement::computeProjectedNoise(Rollout& rollout)
{
  //ros::WallTime start_time = ros::WallTime::now();
  for (int d=0; d<num_dimensions_; ++d)
  {
    rollout.noise_projected_[d] = projection_matrix_[d] * rollout.noise_[d];
    rollout.parameters_noise_projected_[d] = rollout.parameters_[d] + rollout.noise_projected_[d];
  }
  //ROS_INFO("Noise projection took %f seconds", (ros::WallTime::now() - start_time).toSec());
  return true;
}

bool PolicyImprovement::computeRolloutControlCosts()
{
    for (int r=0; r<num_rollouts_; ++r)
    {
        computeRolloutControlCosts(rollouts_[r]);
    }
    return true;
}

bool PolicyImprovement::computeRolloutCumulativeCosts()
{
    // compute cumulative costs at each timestep
    for (int r=0; r<num_rollouts_; ++r)
    {
        for (int d=0; d<num_dimensions_; ++d)
        {
            rollouts_[r].total_costs_[d] = rollouts_[r].state_costs_ + rollouts_[r].control_costs_[d];
            rollouts_[r].cumulative_costs_[d] = rollouts_[r].total_costs_[d];
            if (use_cumulative_costs_)
            {
                for (int t=num_time_steps_-2; t>=0; --t)
                {
                    rollouts_[r].cumulative_costs_[d](t) += rollouts_[r].cumulative_costs_[d](t+1);
                }
            }
        }
    }
    return true;
}

bool PolicyImprovement::computeRolloutProbabilities()
{
    for (int d=0; d<num_dimensions_; ++d)
    {
        //ROS_INFO_STREAM("dimension " << d << ", Cumulative costs " << rollout_cumulative_costs_[d]);
//        tmp_min_cost_ = rollout_cumulative_costs_[d].colwise().minCoeff().transpose();
//        tmp_max_cost_ = rollout_cumulative_costs_[d].colwise().maxCoeff().transpose();
//        tmp_max_minus_min_cost_ = tmp_max_cost_ - tmp_min_cost_;

        for (int t=0; t<num_time_steps_; t++)
        {

            // find min and max cost over all rollouts:
            double min_cost = rollouts_[0].cumulative_costs_[d](t);
            double max_cost = min_cost;
            for (int r=1; r<num_rollouts_; ++r)
            {
                double c = rollouts_[r].cumulative_costs_[d](t);
                if (c < min_cost)
                    min_cost = c;
                if (c > max_cost)
                    max_cost = c;
            }

            double denom = max_cost - min_cost;

            time_step_weights_[d][t] = denom;
            //time_step_weights_[d][t] = 1.0;

            // prevent divide by zero:
            if (denom < 1e-8)
                denom = 1e-8;

            double p_sum = 0.0;
            for (int r=0; r<num_rollouts_; ++r)
            {
                // the -10.0 here is taken from the paper:
                rollouts_[r].probabilities_[d](t) = exp(-10.0*(rollouts_[r].cumulative_costs_[d](t) - min_cost)/denom);
                p_sum += rollouts_[r].probabilities_[d](t);
            }
            for (int r=0; r<num_rollouts_; ++r)
            {
                rollouts_[r].probabilities_[d](t) /= p_sum;
            }

        }

    }
    return true;
}

bool PolicyImprovement::computeParameterUpdates()
{
    for (int d=0; d<num_dimensions_; ++d)
    {
        parameter_updates_[d] = MatrixXd::Zero(num_time_steps_, num_parameters_[d]);

        for (int r=0; r<num_rollouts_; ++r)
        {
            parameter_updates_[d].row(0).transpose() +=
            		(rollouts_[r].noise_[d].array() * rollouts_[r].probabilities_[d].array()).matrix();
        }

        // reweighting the updates per time-step
        double weight = 0.0;
        double weight_sum = 0.0;
        double max_weight = 0.0;
        for (int t=0; t<num_time_steps_; ++t)
        {
          weight = time_step_weights_[d][t];
          weight_sum += weight;
          parameter_updates_[d](0,t) *= weight;
          if (weight > max_weight)
            max_weight = weight;
        }
        if (weight_sum < 1e-6)
          weight_sum = 1e-6;

        double divisor = weight_sum/num_time_steps_;

        if (max_weight > divisor)
        {
          divisor = max_weight;
        }
        parameter_updates_[d].row(0) /= divisor;

        parameter_updates_[d].row(0).transpose() = projection_matrix_[d]*parameter_updates_[d].row(0).transpose();
    }
    return true;
}

bool PolicyImprovement::improvePolicy(std::vector<Eigen::MatrixXd>& parameter_updates)
{
    ROS_ASSERT(initialized_);

    //ros::WallTime start_time = ros::WallTime::now();
    computeRolloutCumulativeCosts();
    //ROS_INFO("Cumulative costs took %f seconds", (ros::WallTime::now() - start_time).toSec());
    //start_time = ros::WallTime::now();
    computeRolloutProbabilities();
    //ROS_INFO("Probabilities took %f seconds", (ros::WallTime::now() - start_time).toSec());
    //start_time = ros::WallTime::now();
    computeParameterUpdates();
    //ROS_INFO("Updates took %f seconds", (ros::WallTime::now() - start_time).toSec());
    parameter_updates = parameter_updates_;

    return true;
}

bool PolicyImprovement::preAllocateTempVariables()
{
    tmp_noise_.clear();
    tmp_parameters_.clear();
    parameter_updates_.clear();
    for (int d=0; d<num_dimensions_; ++d)
    {
        tmp_noise_.push_back(VectorXd::Zero(num_parameters_[d]));
        tmp_parameters_.push_back(VectorXd::Zero(num_parameters_[d]));
        parameter_updates_.push_back(MatrixXd::Zero(num_time_steps_, num_parameters_[d]));
        time_step_weights_.push_back(VectorXd::Zero(num_time_steps_));
    }
    tmp_max_cost_ = VectorXd::Zero(num_time_steps_);
    tmp_min_cost_ = VectorXd::Zero(num_time_steps_);
    tmp_sum_rollout_probabilities_ = VectorXd::Zero(num_time_steps_);

    return true;
}

bool PolicyImprovement::preComputeProjectionMatrices()
{
//  ROS_INFO("Precomputing projection matrices..");
  projection_matrix_.resize(num_dimensions_);
  inv_projection_matrix_.resize(num_dimensions_);
  for (int d=0; d<num_dimensions_; ++d)
  {
    projection_matrix_[d] = inv_control_costs_[d];
    for (int p=0; p<num_parameters_[d]; ++p)
    {
      double column_max = fabs(inv_control_costs_[d](0,p));
      for (int p2 = 1; p2 < num_parameters_[d]; ++p2)
      {
        if (fabs(inv_control_costs_[d](p2,p)) > column_max)
          column_max = fabs(inv_control_costs_[d](p2,p));
      }
      projection_matrix_[d].col(p) *= (1.0/(num_parameters_[d]*column_max));
    }
    //ROS_INFO_STREAM("Projection matrix = \n" << projection_matrix_[d]);
    inv_projection_matrix_[d] = projection_matrix_[d].fullPivLu().inverse();
  }
//  ROS_INFO("Done precomputing projection matrices.");
  return true;
}

//bool PolicyImprovement::addExtraRollouts(std::vector<std::vector<Eigen::VectorXd> >& rollouts, std::vector<Eigen::VectorXd>& rollout_costs)
//{
//    ROS_ASSERT(int(rollouts.size()) == num_rollouts_extra_);
//
//    // update our parameter values, so that the computed noise is correct:
//    ROS_VERIFY(copyParametersFromPolicy());
//
//    for (int r=0; r<num_rollouts_extra_; ++r)
//    {
//        extra_rollouts_[r].parameters_ = rollouts[r];
//        extra_rollouts_[r].state_costs_ = rollout_costs[r];
//        computeNoise(extra_rollouts_[r]);
//        computeProjectedNoise(extra_rollouts_[r]);
//        computeRolloutControlCosts(extra_rollouts_[r]);
//        //ROS_INFO("Extra rollout cost = %f", extra_rollouts_[r].getCost());
//    }
//
//    extra_rollouts_added_ = true;
//    return true;
//}

bool PolicyImprovement::computeNoise(Rollout& rollout)
{
    for (int d=0; d<num_dimensions_; ++d)
    {
        rollout.noise_[d] =  rollout.parameters_noise_[d] - rollout.parameters_[d];
    }
    return true;
}

bool PolicyImprovement::computeRolloutControlCosts(Rollout& rollout)
{
    policy_->computeControlCosts(rollout.parameters_, rollout.noise_projected_,
                                 control_cost_weight_, rollout.control_costs_);
    return true;
}

bool PolicyImprovement::copyParametersFromPolicy()
{
    if (!policy_->getParameters(parameters_))
    {
        ROS_ERROR("Failed to get policy parameters.");
        return false;
    }
    return true;
}

bool PolicyImprovement::getTimeStepWeights(std::vector<Eigen::VectorXd>& time_step_weights)
{
  time_step_weights = time_step_weights_;
  return true;
}

void PolicyImprovement::getAllRollouts(std::vector<Rollout>& rollouts)
{
  rollouts.resize(num_rollouts_);
  for (int i=0; i<num_rollouts_; ++i)
  {
    rollouts[i] = rollouts_[i];
  }
}

};
