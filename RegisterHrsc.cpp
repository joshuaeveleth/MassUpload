

#include <stdio.h>
#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>

#include <vw/Math/Geometry.h>
#include <vw/Math/RANSAC.h>
#include <vw/InterestPoint/InterestData.h>

#include <HrscCommon.h>



struct ErrorMetric {
  double operator() (vw::math::TranslationFittingFunctor::result_type  const& H,
                     vw::Vector3 const& p1,
                     vw::Vector3 const& p2) const {
    double temp =  vw::math::norm_2( p2 - H * p1 );
    //printf("%lf   ", temp);
    return temp;
  }
};




/// Compute an affine transform for the given feature points using
///  the Vision Workbench RANSAC implementation
bool vwRansacAffine(const std::vector<cv::Point2f> &keypointsA, 
                    const std::vector<cv::Point2f> &keypointsB,
                    cv::Mat &transformMatrix,
                    std::vector<int> &inlierIndices)
{
  // Convert points to VW format
  const size_t numPoints = keypointsA.size();
  std::vector<vw::Vector3> vwPtsA(numPoints), vwPtsB(numPoints);
  for (size_t i=0; i<numPoints; ++i)
  {
    vwPtsA[i][0] = keypointsA[i].x;
    vwPtsA[i][1] = keypointsA[i].y;
    vwPtsA[i][2] = 1;
    vwPtsB[i][0] = keypointsB[i].x;
    vwPtsB[i][1] = keypointsB[i].y;
    vwPtsB[i][2] = 1;
    //printf("Pair: (%lf, %lf) ==> (%lf, %lf) \n", vwPtsA[i][0], vwPtsA[i][1], vwPtsB[i][0], vwPtsB[i][1]);
  }

  // Call VW RANSAC function
  typedef vw::math::TranslationFittingFunctor FittingFunctorType;
  //typedef vw::math::InterestPointErrorMetric  ErrorFunctorType;
  typedef ErrorMetric  ErrorFunctorType;
  int    num_iterations         = 100;
  double inlier_threshold       = 2; // Max point distance in pixels
  int    min_num_output_inliers = 20; // Min pixels to count as a match.
  bool   reduce_min_num_output_inliers_if_no_fit = true;
  vw::math::RandomSampleConsensus<FittingFunctorType, ErrorFunctorType> 
      ransac_instance(FittingFunctorType(), ErrorFunctorType(), 
                      num_iterations, inlier_threshold,
                      min_num_output_inliers, reduce_min_num_output_inliers_if_no_fit);
  FittingFunctorType::result_type vwTransform = ransac_instance(vwPtsA, vwPtsB);
  
  // Convert output to OpenCV format
  transformMatrix = cv::Mat(3, 3, CV_32FC1);
  for (int r=0; r<3; ++r)
    for (int c=0; c<3; ++c)
      transformMatrix.at<float>(r, c) = vwTransform[r][c];
  
  //std::cout << transformMatrix << std::endl;
  
  // Manually determine the inlier indices since VW does not do that for us
  std::vector<cv::Point2f> recomputedB;
  inlierIndices.reserve(keypointsA.size());
  cv::perspectiveTransform(keypointsA, recomputedB, transformMatrix);
  for (size_t i=0; i<keypointsA.size(); ++i)
  {
    double dist = cv::norm(keypointsB[i] - recomputedB[i]);
    //std::cout << "Input Pt = " << keypointsB[i] << std::endl;
    //std::cout << "New   Pt = " << recomputedB[i] << std::endl;
    if (dist <= inlier_threshold)
      inlierIndices.push_back(i);
  }
  printf("Found %d inliers.\n", inlierIndices.size());
  
  return true;
}

/// Convenience function for applying a transform to one point
cv::Point2f transformPoint(const cv::Point2f &pointIn, const cv::Mat &transform)
{
  // OpenCV makes us pach the function arguments in to vectors.
  std::vector<cv::Point2f> ptIn(1);
  std::vector<cv::Point2f> ptOut(1);
  ptIn[0] = pointIn;
  cv::perspectiveTransform(ptIn, ptOut, transform);
  return ptOut[0];
}

enum DetectorType {DETECTOR_TYPE_BRISK = 0, 
                   DETECTOR_TYPE_ORB   = 1};

/// Returns the number of inliers
int computeImageTransform(const cv::Mat &refImageIn, const cv::Mat &matchImageIn,
                          const cv::Mat &estimatedTransform, cv::Mat &transform,
                          const std::string debugFolder,
                          const int          kernelSize  =5, 
                          const DetectorType detectorType=DETECTOR_TYPE_ORB)
{

  
  // Preprocess the images to improve feature detection
  cv::Mat refImage, matchImage, temp;
  
  const int scale = 1;
  const int delta = 0;
  cv::Laplacian( refImageIn,   temp, CV_16S, kernelSize, scale, delta, cv::BORDER_DEFAULT );
  cv::convertScaleAbs( temp, refImage, 0.3);
  cv::Laplacian( matchImageIn, temp, CV_16S, kernelSize, scale, delta, cv::BORDER_DEFAULT );
  cv::convertScaleAbs( temp, matchImage, 0.3 );

  //cv::imwrite( "basemapProcessed.jpeg", refImage );
  //cv::imwrite( "nadirProcessed.jpeg", matchImage );
    
  std::vector<cv::KeyPoint> keypointsA, keypointsB;
  cv::Mat descriptorsA, descriptorsB;  

  cv::Ptr<cv::FeatureDetector    > detector;
  cv::Ptr<cv::DescriptorExtractor> extractor;
  switch (detectorType)
  {
    case DETECTOR_TYPE_BRISK:
      detector  = cv::BRISK::create();
      extractor = cv::BRISK::create();
      break;
    case DETECTOR_TYPE_ORB:
      detector  = cv::ORB::create();
      extractor = cv::ORB::create();
      break;
    default: std::cout << "Unrecognized detector!\n"; return false;
  };

  detector->detect(  refImage, keypointsA); // Basemap
  extractor->compute(refImage, keypointsA, descriptorsA);

  detector->detect(  matchImage, keypointsB); // HRSC
  extractor->compute(matchImage, keypointsB, descriptorsB);

  if ( (keypointsA.size() == 0) || (keypointsB.size() == 0) )
  {
    std::cout << "Failed to find any features in an image!\n";
    return 0;
  }

  // Rule out obviously bad matches based on the known starting alignment accuracy
  cv::Mat mask(keypointsA.size(), keypointsB.size(), CV_8UC1);
  const float MAX_MATCH_PIXEL_DISTANCE = 20; 
  size_t numPossibleMatches = 0;
  for (size_t j=0; j<keypointsB.size(); ++j)
  {
    cv::Point2f estRefPoint = transformPoint(keypointsB[j].pt, estimatedTransform);
    for (size_t i=0; i<keypointsA.size(); ++i)
    {
      float distance = cv::norm(keypointsA[i].pt - estRefPoint);
      //std::cout << "D = " << distance << std::endl;
      if (distance < MAX_MATCH_PIXEL_DISTANCE)
      {
        mask.at<uchar>(i,j) = 1;
        numPossibleMatches++;
      }
      else // Too far, disallow a match
        mask.at<uchar>(i,j) = 0;
    } 
  }
  // Make sure we did not throw out too many points due to pruning!
  const size_t MIN_POSSIBLE_POINT_MATCHES = 20;
  if (numPossibleMatches < MIN_POSSIBLE_POINT_MATCHES)
  {
    std::cout << "After pruning there are only " << numPossibleMatches 
              << " possible point matches!\n";
    return 0;  
  }
  
  // Find the closest match for each feature
  //cv::FlannBasedMatcher matcher;
  cv::Ptr<cv::DescriptorMatcher> matcher = cv::DescriptorMatcher::create("BruteForce-Hamming");
  std::vector<cv::DMatch> matches;
  matcher->match(descriptorsA, descriptorsB, matches, mask);

  //-- Quick calculation of max and min distances between keypoints
  double max_dist = 0; double min_dist = 6000;
  for (size_t i=0; i<matches.size(); i++)
  { 
    if ((matches[i].queryIdx < 0) || (matches[i].trainIdx < 0))
      continue;
    double dist = matches[i].distance;
    //std::cout << matches[i].queryIdx <<", "<< matches[i].trainIdx << ", " << dist <<  std::endl;
    if (dist < min_dist) 
      min_dist = dist;
    if (dist > max_dist) 
      max_dist = dist;
  }

  printf("-- Max dist : %f \n", max_dist );
  printf("-- Min dist : %f \n", min_dist );

  //-- Pick out "good" matches
  float goodDist = (min_dist + max_dist) / 2.0;
  //if (argc > 3)
  //  goodDist = atof(argv[3]);
  std::vector< cv::DMatch > good_matches;
  for (int i=0; i<matches.size(); i++)
  { 
    // First verify that the match is valid
    if ( (matches[i].queryIdx < 0) ||
         (matches[i].trainIdx < 0) ||  
         (matches[i].queryIdx >= keypointsA.size()) || 
         (matches[i].trainIdx >= keypointsB.size()) )
      continue;
    // Now check the distance
    if (matches[i].distance < goodDist)
    {
      good_matches.push_back( matches[i]);
    }
  }
  //good_matches = matches;
  printf("Found %d good matches\n", good_matches.size());

  // Compute a transform between the images using RANSAC
  std::vector<cv::Point2f> refPts;
  std::vector<cv::Point2f> matchPts;
  for(size_t i = 0; i < good_matches.size(); i++ )
  {
    // Get the keypoints from the good matches
    refPts.push_back  (keypointsA[good_matches[i].queryIdx].pt);
    matchPts.push_back(keypointsB[good_matches[i].trainIdx].pt);
    //printf("Pair: HRSC(%lf, %lf) ==> NOEL(%lf, %lf),  DIFF = (%lf, %lf)\n", 
    //        matchPts[i].x, matchPts[i].y, refPts[i].x, refPts[i].y,
    //        refPts[i].x-matchPts[i].x, refPts[i].y-matchPts[i].y);
  }
  
  // Compute a transform using the Vision Workbench RANSAC tool
  // Computed transform is from HRSC to REF
  std::vector<int> inlierIndices;
  vwRansacAffine(matchPts, refPts, transform, inlierIndices);
   
  std::vector<cv::Point2f> usedPtsRef, usedPtsMatch;
  for(size_t i = 0; i < inlierIndices.size(); i++ )
  {
    // Get the keypoints from the used matches
    usedPtsRef.push_back  (refPts  [inlierIndices[i]]);
    usedPtsMatch.push_back(matchPts[inlierIndices[i]]);
    //printf("Pair: HRSC(%lf, %lf) ==> NOEL(%lf, %lf),  DIFF = (%lf, %lf)\n", 
    //        matchPts[i].x, matchPts[i].y, refPts[i].x, refPts[i].y,
    //        refPts[i].x-matchPts[i].x, refPts[i].y-matchPts[i].y);
  }
   
  cv::perspectiveTransform(matchPts, refPts, transform);
  for( int i = 0; i < good_matches.size(); i++ )
  {
    //printf("Pair: HRSC(%lf, %lf) ==> NOEL(%lf, %lf),  DIFF = (%lf, %lf)\n", 
    //        matchPts[i].x, matchPts[i].y, refPts[i].x, refPts[i].y,
    //        refPts[i].x-matchPts[i].x, refPts[i].y-matchPts[i].y);  
  }

  cv::Mat matches_image;
  cv::drawMatches( refImageIn, keypointsA, matchImageIn, keypointsB,
                       good_matches, matches_image, cv::Scalar::all(-1), cv::Scalar::all(-1),
                       std::vector<char>(),cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS );
                       

  for (size_t i=0; i<inlierIndices.size(); ++i)
  {
    cv::Point2f matchPt(usedPtsMatch[i] + cv::Point2f(refImage.cols, 0));
    cv::Point2f refPt  (usedPtsRef  [i]); // Point in the reference image
    //printf("OUT Pair: HRSC(%lf, %lf) ==> NOEL(%lf, %lf) \n", ptsIn[i].x, ptsIn[i].y, ptsOut[i].x, ptsOut[i].y);
    //std::cout << "RefPt = " << refPt << std::endl;
    cv::line(matches_image, matchPt, refPt, cv::Scalar(0, 255, 0), 3);
  }
                       
  cv::imwrite( debugFolder+"match_debug_image.tif", matches_image );

  // Return the number of inliers found
  return static_cast<int>(inlierIndices.size());
}

/// Calls computImageTransform with multiple parameters until one succeeds
int computeImageTransformRobust(const cv::Mat &refImageIn, const cv::Mat &matchImageIn,
                                const std::string &debugFolder,
                                const cv::Mat &estimatedTransform, cv::Mat &transform)
{
  // Try not to accept solutions with fewer outliers
  const int DESIRED_NUM_INLIERS  = 10;
  const int REQUIRED_NUM_INLIERS = 3;
  cv::Mat bestTransform;
  int bestNumInliers = 0;
  int numInliers;
  
  // Keep trying transform parameter combinations until we get a good
  //   match as determined by the inlier count
  for (int kernelSize=3; kernelSize<6; kernelSize += 2)
  {
    for (int detectorType=0; detectorType<2; detectorType++)
    {
      printf("Attempting transform with kernel size = %d and detector type = %d\n",
             kernelSize, detectorType);
      numInliers = computeImageTransform(refImageIn, matchImageIn, estimatedTransform, transform, debugFolder,
                                         kernelSize, static_cast<DetectorType>(detectorType));
      if (numInliers >= DESIRED_NUM_INLIERS)
        return numInliers; // This transform is good enough, return it.

      if (numInliers > bestNumInliers)
      {
        // This is the best transform yet.
        bestTransform  = transform;
        bestNumInliers = numInliers;
      }
    } // End detector type loop
  } // End kernel size loop

  if (bestNumInliers < REQUIRED_NUM_INLIERS)
    return 0; // Did not get an acceptable transform!

  // Use the best transform we got
  transform = bestTransform;
  return bestNumInliers;
}

//=============================================================




int main(int argc, char** argv )
{
  
  if (argc < 5)
  {
    printf("usage: RegisterHrsc <Base map path> <HRSC path> <Output path> <Output scale> [<Estimated transform path>]\n");
    return -1;
  }
  std::string refImagePath   = argv[1];
  std::string matchImagePath = argv[2];
  std::string outputPath     = argv[3];
  double outputScale         = atof(argv[4]);
  
  // Load an estimated transform if the user passed one in
  float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  cv::Mat estimatedTransform(3, 3, CV_32FC1, m);
  if (argc == 6)
  {
    std::string estTransformPath = argv[5];
    readTransform(estTransformPath, estimatedTransform);
  }
  
  const int LOAD_GRAY = 0;
  const int LOAD_RGB  = 1;
  
  // Load the input image
  cv::Mat refImageIn = cv::imread(refImagePath, LOAD_GRAY);
  if (!refImageIn.data)
  {
    printf("Failed to load reference image\n");
    return -1;
  }

  cv::Mat matchImageIn = cv::imread(matchImagePath, LOAD_GRAY);
  if (!matchImageIn.data)
  {
    printf("Failed to load match image\n");
    return -1;
  }

  size_t stop = outputPath.rfind("/");
  std::string debugFolder = outputPath.substr(0,stop+1);

  // First compute the transform between the two images
  cv::Mat transform(3, 3, CV_32FC1);
  int numInliers = computeImageTransformRobust(refImageIn, matchImageIn, debugFolder, estimatedTransform, transform);
  if (!numInliers)
  {
    printf("Failed to compute image transform!\n");
    return -1;
  }
  printf("Computed transform with %d inliers.\n", numInliers);
  
 
  // Convert the transform to apply to the higher resolution images
  // - Also convert the transform back into the frame of the full input reference image.
  // - Since we are only computing the translation this is easy!
  transform.at<float>(0, 2) = transform.at<float>(0, 2) * outputScale;
  transform.at<float>(1, 2) = transform.at<float>(1, 2) * outputScale;  
  
  // The output transform is from the HRSC image to the base map
  writeTransform(outputPath, transform);
  
  return 0;
}








