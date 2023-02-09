#include "GSfM_reconstruction_builder.hpp"

#include <glog/logging.h>
#include <memory>
#include <string>
#include <vector>

#include <theia/theia.h>

namespace theia
{

    namespace GSfM
    {

        // Add the view to the reconstruction. If the camera intrinsics group id is set
        // to an invalid group id then simply add the view to the reconstruction without
        // shared camera intrinsics.
        bool AddViewToReconstruction(const std::string &image_filepath,
                                     const CameraIntrinsicsPrior *intrinsics,
                                     const CameraIntrinsicsGroupId intrinsics_group_id,
                                     Reconstruction *reconstruction)
        {
            std::string image_filename;
            CHECK(GetFilenameFromFilepath(image_filepath, true, &image_filename));

            // Add the image to the reconstruction.
            ViewId view_id;
            if (intrinsics_group_id == kInvalidCameraIntrinsicsGroupId)
            {
                view_id = reconstruction->AddView(image_filename);
            }
            else
            {
                view_id = reconstruction->AddView(image_filename, intrinsics_group_id);
            }

            if (view_id == kInvalidViewId)
            {
                LOG(INFO) << "Could not add " << image_filename
                          << " to the reconstruction.";
                return false;
            }

            // Add the camera intrinsics priors if available.
            if (intrinsics != nullptr)
            {
                View *view = reconstruction->MutableView(view_id);
                *view->MutableCameraIntrinsicsPrior() = *intrinsics;
            }
            return true;
        }

        Reconstruction *CreateEstimatedSubreconstruction(
            const Reconstruction &input_reconstruction)
        {
            std::unique_ptr<Reconstruction> subreconstruction(
                new Reconstruction(input_reconstruction));
            const auto &view_ids = subreconstruction->ViewIds();
            for (const ViewId view_id : view_ids)
            {
                const View *view = subreconstruction->View(view_id);
                if (view == nullptr)
                {
                    continue;
                }

                if (!view->IsEstimated())
                {
                    subreconstruction->RemoveView(view_id);
                }
            }

            const auto &track_ids = subreconstruction->TrackIds();
            for (const TrackId track_id : track_ids)
            {
                const Track *track = subreconstruction->Track(track_id);
                if (track == nullptr)
                {
                    continue;
                }

                if (!track->IsEstimated())
                {
                    subreconstruction->RemoveTrack(track_id);
                }
            }
            return subreconstruction.release();
        }

        void RemoveEstimatedViewsAndTracks(Reconstruction *reconstruction,
                                           ViewGraph *view_graph)
        {
            const auto &view_ids = reconstruction->ViewIds();
            for (const ViewId view_id : view_ids)
            {
                const View *view = reconstruction->View(view_id);
                if (view == nullptr)
                {
                    continue;
                }

                if (view->IsEstimated())
                {
                    reconstruction->RemoveView(view_id);
                    view_graph->RemoveView(view_id);
                }
            }

            const auto &track_ids = reconstruction->TrackIds();
            for (const TrackId track_id : track_ids)
            {
                const Track *track = reconstruction->Track(track_id);
                if (track == nullptr)
                {
                    continue;
                }

                if (track->IsEstimated())
                {
                    reconstruction->RemoveTrack(track_id);
                }
            }
        }

    } // namespace


    GSfMReconstructionBuilder::GSfMReconstructionBuilder(
        const ReconstructionBuilderOptions &options,
        Reconstruction* reconstruction,
        ViewGraph* view_graph)
        : options_(options),
          reconstruction_(reconstruction),
          view_graph_(view_graph)
    {
        CHECK_GT(options.num_threads, 0);
        options_.reconstruction_estimator_options.rng = options.rng;
    }

    GSfMReconstructionBuilder::GSfMReconstructionBuilder(
        const ReconstructionBuilderOptions &options,
        FeaturesAndMatchesDatabase *features_and_matches_database)
        : options_(options),
          features_and_matches_database_(features_and_matches_database)
    {
        CHECK_GT(options.num_threads, 0);

        options_.reconstruction_estimator_options.rng = options.rng;

        reconstruction_ = new Reconstruction();
        view_graph_ = new ViewGraph();
        contains_allocated_objects = true;
        // reconstruction_.reset(new Reconstruction());
        // view_graph_.reset(new ViewGraph());
        track_builder_.reset(
            new TrackBuilder(options.min_track_length, options.max_track_length));

        // Set up feature extraction and matching.
        FeatureExtractorAndMatcher::Options feam_options;
        feam_options.num_threads = options_.num_threads;
        feam_options.only_calibrated_views = options_.only_calibrated_views;
        feam_options.num_threads = options_.num_threads;
        feam_options.descriptor_extractor_type = options_.descriptor_type;
        feam_options.feature_density = options_.feature_density;
        feam_options.min_num_inlier_matches = options_.min_num_inlier_matches;
        feam_options.matching_strategy = options_.matching_strategy;
        feam_options.feature_matcher_options = options_.matching_options;
        feam_options.feature_matcher_options.geometric_verification_options
            .min_num_inlier_matches = options_.min_num_inlier_matches;
        feam_options.feature_matcher_options.geometric_verification_options
            .estimate_twoview_info_options.rng = options_.rng;

        // Global descriptor matching settings.
        feam_options.select_image_pairs_with_global_image_descriptor_matching =
            options_.select_image_pairs_with_global_image_descriptor_matching;
        feam_options.num_nearest_neighbors_for_global_descriptor_matching =
            options_.num_nearest_neighbors_for_global_descriptor_matching;
        feam_options.num_gmm_clusters_for_fisher_vector =
            options_.num_gmm_clusters_for_fisher_vector;
        feam_options.max_num_features_for_fisher_vector_training =
            options_.max_num_features_for_fisher_vector_training;

        feature_extractor_and_matcher_.reset(new FeatureExtractorAndMatcher(
            feam_options, features_and_matches_database_));
    }

    GSfMReconstructionBuilder::~GSfMReconstructionBuilder() {
        if(contains_allocated_objects){
            delete reconstruction_;
            delete view_graph_;
        }
    }

    bool GSfMReconstructionBuilder::AddImage(const std::string &image_filepath)
    {
        return AddImage(image_filepath, kInvalidCameraIntrinsicsGroupId);
    }

    bool GSfMReconstructionBuilder::AddImage(
        const std::string &image_filepath,
        const CameraIntrinsicsGroupId camera_intrinsics_group)
    {
        image_filepaths_.emplace_back(image_filepath);
        if (!GSfM::AddViewToReconstruction(image_filepath,
                                                 NULL,
                                                 camera_intrinsics_group,
                                                 reconstruction_))
        {
            return false;
        }
        return feature_extractor_and_matcher_->AddImage(image_filepath);
    }

    bool GSfMReconstructionBuilder::AddImageWithCameraIntrinsicsPrior(
        const std::string &image_filepath,
        const CameraIntrinsicsPrior &camera_intrinsics_prior)
    {
        return AddImageWithCameraIntrinsicsPrior(
            image_filepath, camera_intrinsics_prior, kInvalidCameraIntrinsicsGroupId);
    }

    bool GSfMReconstructionBuilder::AddImageWithCameraIntrinsicsPrior(
        const std::string &image_filepath,
        const CameraIntrinsicsPrior &camera_intrinsics_prior,
        const CameraIntrinsicsGroupId camera_intrinsics_group)
    {
        image_filepaths_.emplace_back(image_filepath);
        if (!GSfM::AddViewToReconstruction(image_filepath,
                                                 &camera_intrinsics_prior,
                                                 camera_intrinsics_group,
                                                 reconstruction_))
        {
            return false;
        }
        return feature_extractor_and_matcher_->AddImage(image_filepath,
                                                        camera_intrinsics_prior);
    }

    void GSfMReconstructionBuilder::RemoveUncalibratedViews()
    {
        const auto &view_ids = reconstruction_->ViewIds();
        for (const ViewId view_id : view_ids)
        {
            const View *view = reconstruction_->View(view_id);
            if (!view->CameraIntrinsicsPrior().focal_length.is_set)
            {
                reconstruction_->RemoveView(view_id);
                view_graph_->RemoveView(view_id);
            }
        }
    }

    bool GSfMReconstructionBuilder::AddMaskForFeaturesExtraction(
        const std::string &image_filepath, const std::string &mask_filepath)
    {
        feature_extractor_and_matcher_->AddMaskForFeaturesExtraction(image_filepath,
                                                                     mask_filepath);
        return true;
    }

    bool GSfMReconstructionBuilder::ExtractAndMatchFeatures()
    {
        CHECK_EQ(view_graph_->NumViews(), 0) << "Cannot call ExtractAndMatchFeatures "
                                                "after TwoViewMatches has been "
                                                "called.";

        // TODO: Remove all references to matches variable and replace with db
        // functions.

        // Extract features and obtain the feature matches.
        feature_extractor_and_matcher_->ExtractAndMatchFeatures();
        feature_extractor_and_matcher_.release();

        // Log how many view pairs were geometrically verified.
        const int num_total_view_pairs =
            image_filepaths_.size() * (image_filepaths_.size() - 1) / 2;
        LOG(INFO) << features_and_matches_database_->NumMatches() << " of "
                  << num_total_view_pairs
                  << " view pairs were matched and geometrically verified.";

        // Add the EXIF metadata to each view.
        std::vector<std::string> image_filenames(image_filepaths_.size());
        const auto image_names_of_calibration =
            features_and_matches_database_->ImageNamesOfCameraIntrinsicsPriors();
        for (int i = 0; i < image_names_of_calibration.size(); i++)
        {
            // Add the camera intrinsic prior information to the view.
            const ViewId view_id =
                reconstruction_->ViewIdFromName(image_names_of_calibration[i]);
            View *view = reconstruction_->MutableView(view_id);
            const auto intrinsics_prior =
                features_and_matches_database_->GetCameraIntrinsicsPrior(
                    image_names_of_calibration[i]);
            *view->MutableCameraIntrinsicsPrior() = intrinsics_prior;
        }

        // Add the matches to the view graph and reconstruction.
        const auto &match_keys =
            features_and_matches_database_->ImageNamesOfMatches();
        for (const auto &match_key : match_keys)
        {
            const ImagePairMatch &match =
                features_and_matches_database_->GetImagePairMatch(match_key.first,
                                                                  match_key.second);
            AddTwoViewMatch(match_key.first, match_key.second, match);
        }

        return true;
    }

    bool GSfMReconstructionBuilder::AddTwoViewMatch(const std::string &image1,
                                                          const std::string &image2,
                                                          const ImagePairMatch &matches)
    {
        // Get view ids from names and check that the views are valid (i.e. that
        // they have been added to the reconstruction).
        const ViewId view_id1 = reconstruction_->ViewIdFromName(image1);
        const ViewId view_id2 = reconstruction_->ViewIdFromName(image2);
        CHECK_NE(view_id1, kInvalidViewId)
            << "Tried to add a view with the name " << image1
            << " to the view graph but does not exist in the reconstruction.";
        CHECK_NE(view_id2, kInvalidViewId)
            << "Tried to add a view with the name " << image2
            << " to the view graph but does not exist in the reconstruction.";

        // If we only want calibrated views, do not add the match if it contains an
        // uncalibrated view since it will add uncalibrated views to the tracks.
        const View *view1 = reconstruction_->View(view_id1);
        const View *view2 = reconstruction_->View(view_id2);
        if (options_.only_calibrated_views &&
            (!view1->CameraIntrinsicsPrior().focal_length.is_set ||
             !view2->CameraIntrinsicsPrior().focal_length.is_set))
        {
            return true;
        }

        // Add valid matches to view graph.
        AddMatchToViewGraph(view_id1, view_id2, matches);

        // Add tracks to the track builder.
        AddTracksForMatch(view_id1, view_id2, matches);

        return true;
    }

    bool GSfMReconstructionBuilder::BuildReconstruction(
        std::vector<Reconstruction *> *reconstructions)
    {

        CheckView(reconstructions);

        while (reconstruction_->NumViews() > 1)
        {
            LOG(INFO) << "Attempting to reconstruct " << reconstruction_->NumViews()
                      << " images from " << view_graph_->NumEdges()
                      << " two view matches.";

            std::unique_ptr<GSfMGlobalReconstructionEstimator> reconstruction_estimator(
                new GSfMGlobalReconstructionEstimator(options_.reconstruction_estimator_options));

            const auto &summary = reconstruction_estimator->Estimate(
                view_graph_, reconstruction_);

            // If a reconstruction can no longer be estimated, return.
            if (!summary.success)
            {
                return reconstructions->size() > 0;
            }

            LOG(INFO) << "\nReconstruction estimation statistics: "
                      << "\n\tNum estimated views = " << summary.estimated_views.size()
                      << "\n\tNum input views = " << reconstruction_->NumViews()
                      << "\n\tNum estimated tracks = "
                      << summary.estimated_tracks.size()
                      << "\n\tNum input tracks = " << reconstruction_->NumTracks()
                      << "\n\tPose estimation time = " << summary.pose_estimation_time
                      << "\n\tTriangulation time = " << summary.triangulation_time
                      << "\n\tBundle Adjustment time = "
                      << summary.bundle_adjustment_time
                      << "\n\tTotal time = " << summary.total_time << "\n\n"
                      << summary.message;

            // Remove estimated views and tracks and attempt to create a reconstruction
            // from the remaining unestimated parts.
            reconstructions->emplace_back(
                GSfM::CreateEstimatedSubreconstruction(*reconstruction_));
            GSfM::RemoveEstimatedViewsAndTracks(reconstruction_, view_graph_);

            // Exit after the first reconstruction estimation if only the single largest
            // reconstruction is desired.
            if (options_.reconstruct_largest_connected_component)
            {
                return reconstructions->size() > 0;
            }

            if (reconstruction_->NumViews() < 3)
            {
                LOG(INFO) << "No more reconstructions can be estimated.";
                return reconstructions->size() > 0;
            }
        }
        return true;
    }

    void GSfMReconstructionBuilder::AddMatchToViewGraph(
        const ViewId view_id1,
        const ViewId view_id2,
        const ImagePairMatch &image_matches)
    {
        // Add the view pair to the reconstruction. The view graph requires the two
        // view info to specify the transformation from the smaller view id to the
        // larger view id. We swap the cameras here if that is not already the case.
        TwoViewInfo twoview_info = image_matches.twoview_info;
        if (view_id1 > view_id2)
        {
            SwapCameras(&twoview_info);
        }

        view_graph_->AddEdge(view_id1, view_id2, twoview_info);
    }

    void GSfMReconstructionBuilder::AddTracksForMatch(const ViewId view_id1,
                                                            const ViewId view_id2,
                                                            const ImagePairMatch &matches)
    {
        for (const auto &match : matches.correspondences)
        {
            track_builder_->AddFeatureCorrespondence(
                view_id1, match.feature1, view_id2, match.feature2);
        }
    }

    void GSfMReconstructionBuilder::CheckView(std::vector<Reconstruction *> *reconstructions)
    {
        CHECK_GE(view_graph_->NumViews(), 2) << "At least 2 images must be provided "
                                                "in order to create a "
                                                "reconstruction.";

        // Build tracks if they were not explicitly specified.
        if (reconstruction_->NumTracks() == 0)
        {
            track_builder_->BuildTracks(reconstruction_);
        }

        // Remove uncalibrated views from the reconstruction and view graph.
        if (options_.only_calibrated_views)
        {
            LOG(INFO) << "Removing uncalibrated views.";
            RemoveUncalibratedViews();
        }
    }

} // namespace theia