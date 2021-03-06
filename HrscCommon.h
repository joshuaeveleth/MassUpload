

#include <stdio.h>
#include <sstream>
#include <opencv2/opencv.hpp>

#include <vw/Math/Geometry.h>
#include <vw/Math/RANSAC.h>
#include <vw/InterestPoint/InterestData.h>

#include <vw/FileIO/DiskImageView.h>
#include <vw/FileIO/DiskImageResourceGDAL.h>
#include <vw/FileIO/DiskImageUtils.h>
#include <vw/Cartography/GeoReference.h>

const size_t NUM_HRSC_CHANNELS = 5;
const size_t NUM_BASE_CHANNELS = 3;

typedef unsigned short MASK_DATA_TYPE;
typedef unsigned char  BINARY_MASK_DATA_TYPE;

//const unsigned char MASK_MAX = 255; // UINT8
const unsigned short MASK_MAX = 1023; // UINT16 - This is equal to the grassfire distance!



/// Constrain an OpenCV ROI to lie within an image
/// - Returns false if there is no overlap
bool constrainCvRoi(cv::Rect &roi, const int imageWidth, const int imageHeight)
{
  //std::cout << "roi    = " << roi << std::endl;
  //std::cout << "width  = " << imageWidth << std::endl;
  //std::cout << "height = " << imageHeight << std::endl;
  cv::Rect imageRoi(0, 0, imageWidth, imageHeight);
  roi &= imageRoi;
  return (roi.area() > 0);
}

/// As constrainCvRoi, but also resizes roi2 to match the changes to roi1.
bool constrainMatchedCvRois(cv::Rect &roi, const int imageWidth, const int imageHeight,
                            cv::Rect &roi2)
{
  // Constrain the first ROI
  cv::Rect roiIn = roi;
  if (!constrainCvRoi(roi, imageWidth, imageHeight))
    return false;
  // Detect the changes
  cv::Point tlDiff = roi.tl()   - roiIn.tl(); // TL corner can only have increased
  //cv::Point brDiff = roiIn.br() - roi.br();
  
  //std::cout << "tlDiff  = " << tlDiff << std::endl;
  
  roi2 = cv::Rect(roi2.tl() + tlDiff, roi.size()); // Use the new size
  //std::cout << "roi2  = " << roi2<< std::endl;
  return true;
}


std::string itoa(const int i)
{
  std::stringstream s;
  s << i;
  return s.str();
}

void affineTransform(const cv::Mat &transform, float xIn, float yIn, float &xOut, float &yOut)
{
  xOut = xIn*transform.at<float>(0,0) + yIn*transform.at<float>(0,1) + transform.at<float>(0,2);
  yOut = xIn*transform.at<float>(1,0) + yIn*transform.at<float>(1,1) + transform.at<float>(1,2);
}

/// Single channel image interpolation
template <typename T>
T interpPixel(const cv::Mat& img, const cv::Mat& mask, float xF, float yF, bool &gotValue)
{
  const int BORDER_SIZE = 1; // Stay away from border artifacts

  gotValue = false;
  int x = (int)xF;
  int y = (int)yF;

  // Get the bordering pixel coordinates, replacing out of bounds with zero.
  int minX = BORDER_SIZE; // Max legal pixel boundaries with the specified border.
  int minY = BORDER_SIZE;
  int maxX = img.cols-BORDER_SIZE;
  int maxY = img.rows-BORDER_SIZE;
  int x0 = x;   // The coordinates of the four bordering pixels
  int x1 = x+1;
  int y0 = y;
  int y1 = y+1;
  if ((x0 < minX) || (x0 >= maxX)) return 0; // Quit if we exceed any of the borders.
  if ((x1 < minX) || (x1 >= maxX)) return 0;
  if ((y0 < minY) || (y0 >= maxY)) return 0;
  if ((y1 < minY) || (y1 >= maxY)) return 0;
  
  // - Don't interpolate if any mask inputs are zero, this might indicate 
  //    that we are at a projection border.
  unsigned char i00 = mask.at<MASK_DATA_TYPE>(y0, x0);
  unsigned char i01 = mask.at<MASK_DATA_TYPE>(y0, x1);
  unsigned char i10 = mask.at<MASK_DATA_TYPE>(y1, x0);
  unsigned char i11 = mask.at<MASK_DATA_TYPE>(y1, x1);
  if ((i00 == 0) || (i01 == 0) || (i10 == 0) || (i11 == 0))
    return 0;


  float a = xF - (float)x;
  float c = yF - (float)y;
  
  float v00 = static_cast<float>(img.at<T>(y0, x0));
  float v01 = static_cast<float>(img.at<T>(y0, x1));
  float v10 = static_cast<float>(img.at<T>(y1, x0));
  float v11 = static_cast<float>(img.at<T>(y1, x1));

  T val = static_cast<T>( v00*(1-a)*(1-c)  + v10*a*(1-c) + v01*(1-a)*c + v11*a*c );

  gotValue = true;
  return val;
}

/// As interpPixel but specialized for RGB
cv::Vec3b interpPixelRgb(const cv::Mat& img, float xF, float yF, bool &gotValue)
{
  const size_t NUM_RGB_CHANNELS = 3;
  const int    BORDER_SIZE      = 1; // Stay away from border artifacts

  gotValue = false;
  int x = (int)xF;
  int y = (int)yF;

  // Get the bordering pixel coordinates, replacing out of bounds with zero.
  int minX = BORDER_SIZE;
  int minY = BORDER_SIZE;
  int maxX = img.cols-BORDER_SIZE;
  int maxY = img.rows-BORDER_SIZE;
  int x0 = x;
  int x1 = x+1;
  int y0 = y;
  int y1 = y+1;
  if ((x0 < minX) || (x0 >= maxX)) return 0;
  if ((x1 < minX) || (x1 >= maxX)) return 0;
  if ((y0 < minY) || (y0 >= maxY)) return 0;
  if ((y1 < minY) || (y1 >= maxY)) return 0;

  // Now interpolate each pixel channel

  float a = xF - (float)x;
  float c = yF - (float)y;
  
  cv::Vec3b outputPixel;
  for (size_t i=0; i<NUM_RGB_CHANNELS; ++i)
  {
    float v00 = static_cast<float>(img.at<cv::Vec3b>(y0, x0)[i]);
    float v01 = static_cast<float>(img.at<cv::Vec3b>(y0, x1)[i]);
    float v10 = static_cast<float>(img.at<cv::Vec3b>(y1, x0)[i]);
    float v11 = static_cast<float>(img.at<cv::Vec3b>(y1, x1)[i]);

    outputPixel[i] = static_cast<unsigned char>( v00*(1-a)*(1-c)  + v10*a*(1-c) + v01*(1-a)*c + v11*a*c );

  }

  gotValue = true;
  return outputPixel;
}

/// As interpPixelRgb but with pixels near the edges handled by mirroring
template <typename MASK_T>
cv::Vec3b interpPixelMirrorRgb(const cv::Mat& img,  const cv::Mat& mask,
                               float xF, float yF, bool &gotValue)
{
  const size_t NUM_RGB_CHANNELS = 3;

  // Get the bounding pixel coordinates
  gotValue = false;
  int x = (int)xF;
  int y = (int)yF;
  int x0 = x;
  int x1 = x+1;
  int y0 = y;
  int y1 = y+1;
  /*
  // Mirror a border of one by adjusting the bounding coordinates
  if (x0 == -1)       x0 = 0;
  if (y0 == -1)       y0 = 0;
  if (x1 == img.cols) x1 = img.cols-1;
  if (y1 == img.rows) y1 = img.rows-1;
  */
  // Pixels past the border are still rejected
  if ((x0 < 0) || (x0 >= img.cols)) return 0;
  if ((x1 < 0) || (x1 >= img.cols)) return 0;
  if ((y0 < 0) || (y0 >= img.rows)) return 0;
  if ((y1 < 0) || (y1 >= img.rows)) return 0;

  // Check the mask
  // - Don't interpolate if any mask inputs are zero, this might indicate 
  //    that we are at a projection border.
  unsigned char i00 = mask.at<MASK_T>(y0, x0);
  unsigned char i01 = mask.at<MASK_T>(y0, x1);
  unsigned char i10 = mask.at<MASK_T>(y1, x0);
  unsigned char i11 = mask.at<MASK_T>(y1, x1);
  if ((i00 == 0) || (i01 == 0) || (i10 == 0) || (i11 == 0))
    return 0;
  
  // Now interpolate each pixel channel

  float a = xF - (float)x;
  float c = yF - (float)y;
  
  cv::Vec3b outputPixel;
  for (size_t i=0; i<NUM_RGB_CHANNELS; ++i)
  {
    float v00 = static_cast<float>(img.at<cv::Vec3b>(y0, x0)[i]);
    float v01 = static_cast<float>(img.at<cv::Vec3b>(y0, x1)[i]);
    float v10 = static_cast<float>(img.at<cv::Vec3b>(y1, x0)[i]);
    float v11 = static_cast<float>(img.at<cv::Vec3b>(y1, x1)[i]);  
    outputPixel[i] = static_cast<unsigned char>( v00*(1.0f-a)*(1.0f-c)  + v10*a*(1.0f-c) + v01*(1.0f-a)*c + v11*a*c );
  }
  
  gotValue = true;
  return outputPixel;
}


/// Computes the ROI of one image in another given the transform with bounds checking.
cv::Rect_<int> getboundsInOtherImage(const cv::Mat &imageA, const cv::Mat &imageB, const cv::Mat &transB_to_A)
{
  // Transform the four corners of imageB
  float x[4], y[4];
  affineTransform(transB_to_A, 0,             0,             x[0], y[0]);
  affineTransform(transB_to_A, imageB.cols-1, 0,             x[1], y[1]);
  affineTransform(transB_to_A, imageB.cols-1, imageB.rows-1, x[2], y[2]);
  affineTransform(transB_to_A, 0,             imageB.rows-1, x[3], y[3]);
  
  // Get the bounding box of the transformed points
  float xMin = x[0];
  float xMax = x[0];
  float yMin = y[0];
  float yMax = y[0];
  for (size_t i=0; i<4; ++i)
  {
    if (x[i] < xMin) xMin = x[i];
    if (x[i] > xMax) xMax = x[i];
    if (y[i] < yMin) yMin = y[i];
    if (y[i] > yMax) yMax = y[i];
  }
  
  if (xMin < 0) xMin = 0;
  if (yMin < 0) yMin = 0;
  if (xMax > imageA.cols-1) xMax = imageA.cols-1;
  if (yMax > imageA.rows-1) yMax = imageA.rows-1;

  // Return the results expanded to the nearest integer
  cv::Rect_<int> boundsInA(static_cast<int>(floor(xMin)), 
                           static_cast<int>(floor(yMin)),
                           static_cast<int>(ceil(xMax-xMin)), 
                           static_cast<int>(ceil(yMax-yMin)));
  return boundsInA;
}

/// Write a small matrix to a text file
bool writeTransform(const std::string &outputPath, const cv::Mat &transform)
{
  std::ofstream file(outputPath.c_str());
  file << transform.rows << ", " << transform.cols << std::endl;
  for (size_t r=0; r<transform.rows; ++r)
  {
    for (size_t c=0; c<transform.cols-1; ++c)
    {
      file << transform.at<float>(r,c) << ", ";
    }
    file << transform.at<float>(r,transform.cols-1) << std::endl;
  }
  file.close();
  
  return (!file.fail());
}

// Read a small matrix from a text file
bool readTransform(const std::string &inputPath, cv::Mat &transform)
{
  //printf("Reading transform: %s\n", inputPath.c_str());
  std::ifstream file(inputPath.c_str());
  if (!file.fail())
  {
    char   comma;
    size_t numRows, numCols;
    file >> numRows >> comma >> numCols;
    transform.create(numRows, numCols, CV_32FC1);
    for (size_t r=0; r<transform.rows; ++r)
    {
      for (size_t c=0; c<transform.cols-1; ++c)
      {
        file >> transform.at<float>(r,c) >> comma;
      }
      file >> transform.at<float>(r,transform.cols-1);
    }
    file.close();
  }
  if (file.fail())
  {
    std::cout << "Failed to load transform file: " << inputPath << std::endl;
    return false;
  }
  return true;
}

/// Try to load the image and then make sure we got valid data.
/// - The type must by 0 (gray) or 1 (RGB)
bool readOpenCvImage(const std::string &imagePath, cv::Mat &image, const int imageType)
{
  //printf("Reading image file: %s\n", imagePath.c_str());
  image = cv::imread(imagePath, imageType | CV_LOAD_IMAGE_ANYDEPTH);
  if (!image.data)
  {
    printf("Failed to load image %s!\n", imagePath.c_str());
    return false;
  }
  return true;
}


/// Helper class for working with brightness information
class BrightnessCorrector
{
public:

  /// Data loading options
  BrightnessCorrector() {}
  BrightnessCorrector(cv::Mat &gain, cv::Mat &offset) : _gain(gain), _offset(offset) { }
  void set(cv::Mat &gain, cv::Mat &offset) { _gain = gain; _offset = offset; }

  /// Write a gain/offset pair to a CSV file
  bool writeProfileCorrection(const std::string &outputPath) const
  {
    std::ofstream file(outputPath.c_str());
    file << _gain.rows << std::endl;
    for (size_t r=0; r<_gain.rows; ++r)
    {
      file << _gain.at<float>(r,0) << ", " << _offset.at<float>(r,0) << std::endl;
    }
    file.close();
    return (!file.fail());
  }

  /// Write a gain/offset pair to a CSV file
  bool readProfileCorrection(const std::string &inputPath)
  {
    //std::cout << "Reading profile correction: " << inputPath << std::endl;
    std::ifstream file(inputPath.c_str());
    if (!file.fail())
    {
      char   comma;
      size_t numRows;
      file >> numRows;
      _gain.create(numRows, 1, CV_32FC1);
      _offset.create(numRows, 1, CV_32FC1);
      for (size_t r=0; r<numRows; ++r)
      {
        file >> _gain.at<float>(r,0) >> comma >> _offset.at<float>(r,0);
      }
      file.close();
    }
    if (file.fail())
    {
      std::cout << "Failed to load profile correction: " << inputPath << std::endl;
      return false;
    }
    return true;
  }

  /// Get the corrected value of a single pixel
  unsigned char correctPixel(unsigned char inputPixel, int row) const
  {
    float result = static_cast<float>(inputPixel) * _gain.at<float>(row,0);
    if (result <   0.0) result = 0.0;  // Clamp the output value
    if (result > 255.0) result = 255.0;
    return static_cast<unsigned char>(result);
  }
  
private:

  cv::Mat _gain;
  cv::Mat _offset;
};





/// Replace the Value channel of the input HSV image
bool replaceValue(const cv::Mat &baseImageRgb, const cv::Mat &spatialTransform, const cv::Mat &nadir, cv::Mat &outputImage)
{
  printf("Converting image...\n");
  
  // Convert the input image to HSV
  cv::Mat hsvImage;
  cv::cvtColor(baseImageRgb, hsvImage, cv::COLOR_BGR2HSV);
 
  printf("Replacing value channel...\n");
 
  // TODO: There must be a better way to do this using OpenCV!
  // Replace the value channel
  //cv::Mat outputMask;
  bool gotValue;
  for (int r=0; r<baseImageRgb.rows; ++r)
  {
    for (int c=0; c<baseImageRgb.cols; ++c)
    {     
      float matchX = c*spatialTransform.at<float>(0,0) + r*spatialTransform.at<float>(0,1) + spatialTransform.at<float>(0,2);
      float matchY = c*spatialTransform.at<float>(1,0) + r*spatialTransform.at<float>(1,1) + spatialTransform.at<float>(1,2);
      
      unsigned char newVal = interpPixel<unsigned char>(nadir, nadir, matchX, matchY, gotValue);
      //hsvImage.at<unsigned char>(r,c, 2) = newVal;
      if (gotValue)
        hsvImage.at<cv::Vec3b>(r,c)[2] = newVal;
    }
  }
  cv::cvtColor(hsvImage, outputImage, cv::COLOR_HSV2BGR);
  
  cv::imwrite("value_replaced_image.jpeg", outputImage);
  
  //cv::namedWindow("Display Image", cv::WINDOW_AUTOSIZE );
  //cv::imshow("Display Image", outputImage);
  //cv::waitKey(0);
  
  printf("Finished replacing value\n");
  return true;

}


/// Converts a single RGB pixel to YCbCr
cv::Vec3b rgb2ycbcr(cv::Vec3b rgb)
{
  // Convert
  double temp[3];
  temp[0] =         0.299   *rgb[0] + 0.587   *rgb[1] + 0.114   *rgb[2];
  temp[1] = 128.0 - 0.168736*rgb[0] - 0.331264*rgb[1] + 0.5     *rgb[2];
  temp[2] = 128.0 + 0.5     *rgb[0] - 0.418688*rgb[1] - 0.081312*rgb[2];
  // Copy and constrain
  cv::Vec3b ycbcr;
  for (int i=0; i<3; ++i)
  {
    ycbcr[i] = temp[i];
    if (temp[i] < 0.0  ) ycbcr[i] = 0;
    if (temp[i] > 255.0) ycbcr[i] = 255;
  }
  return ycbcr;
}
    
/// Converts a single YCbCr pixel to RGB
cv::Vec3b ycbcr2rgb(cv::Vec3b ycbcr)
{
  double temp[3];
  temp[0] = ycbcr[0]                                + 1.402   * (ycbcr[2] - 128.0);
  temp[1] = ycbcr[0] - 0.34414 * (ycbcr[1] - 128.0) - 0.71414 * (ycbcr[2] - 128.0);
  temp[2] = ycbcr[0] + 1.772   * (ycbcr[1] - 128.0);
  
  // Copy and constrain
  cv::Vec3b rgb;
  for (int i=0; i<3; ++i)
  {
    rgb[i] = temp[i];
    if (temp[i] < 0.0  ) rgb[i] = 0;
    if (temp[i] > 255.0) rgb[i] = 255;
  }
  return rgb;
}


//=========================================================================================
// Functions copied from ASP
// - Maybe these functions should live in Vision Workbench?

template <class ImageT>
vw::DiskImageResourceGDAL*
build_gdal_rsrc( const std::string &filename,
               vw::ImageViewBase<ImageT> const& image) 
{
  vw::DiskImageResourceGDAL::Options gdal_options;

  // If the image is big, make sure we write bigtiff format.
  //if ( (disk_image.cows() > 30000) && (disk_image.rows() > 15000))
  gdal_options["BIGTIFF"] = "IF_SAFER";//"YES";


  // The tile size is hardcoded to a good number!
  vw::Vector2i raster_tile_size(1024, 1024);
  return new vw::DiskImageResourceGDAL(filename, image.impl().format(), raster_tile_size, gdal_options);
}


// Block write image with georef and keywords to geoheader.
template <class ImageT>
void block_write_gdal_image( const std::string &filename,
                             vw::ImageViewBase<ImageT> const& image,
                             vw::cartography::GeoReference const& georef,
                             vw::ProgressCallback const& progress_callback =                     
                                               vw::ProgressCallback::dummy_instance(),
                             std::map<std::string, std::string> keywords =
                                               std::map<std::string, std::string>()
                           ) 
{
  boost::scoped_ptr<vw::DiskImageResourceGDAL> rsrc( build_gdal_rsrc( filename, image ) );
  for (std::map<std::string, std::string>::iterator i = keywords.begin(); i != keywords.end(); i++)
  {
    vw::cartography::write_header_string(*rsrc, i->first, i->second);
  }
  vw::cartography::write_georeference(*rsrc, georef);
  vw::block_write_image( *rsrc, image.impl(), progress_callback );
}






