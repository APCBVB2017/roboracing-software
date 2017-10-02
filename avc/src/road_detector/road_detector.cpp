#include <ros/ros.h>
#include <ros/publisher.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/Image.h>
#include "opencv2/imgproc/imgproc.hpp"
#include <opencv2/highgui/highgui.hpp>

using namespace std;

ros::Publisher pub;


/*
	This Histogram Road Detection Algorithmn is based on:
	http://www.morethantechnical.com/2013/03/05/skin-detection-with-probability-maps-and-elliptical-boundaries-opencv-wcode/
	http://www.cse.unsw.edu.au/~icml2002/workshops/MLCV02/MLCV02-Moral:11311/

es.pdf
	http://www.sciencedirect.com/science/article/pii/S0031320306002767

	Be careful, a bad bootstrap will cause it to converge around non road!
*/

//##########################################
// Bootstrap is the "first guess". This can be done by a previously made histogram or selection of values. Needs to be right!
// Train is normalizing the histogram compared to the number of pixels the mask finds. Allows for some "learning" to happen.
// Predict is using the histograms this program created to decide which pixels are road

//Constants to play with
const float road_threshold_bootstrap = 0.09; //worksforAVCbag: 0.12; 
const float shadows_threshold_bootstrap = 0.1;//worksForAVCbag: 0.091;

const float road_threshold = 2.3; //WORKSForAVCbag 2.7;
const float shadows_threshold = 0.53; //WORKSFORAVCbag 2.1 or 1.9;

//This is here to make sure it isn't a magic string, but shouldn't need to be changed.
const string road_histogram_to_load = "road_histogram_hue_normalized"; //Name of histogram and file (don't include file type)
const string road_histogram_to_load = "shadows_histogram_hue_normalized"; //Name of histogram and file (don't include file type)

//0 = off; 1 = normal (good for most things); 2 = medium (similar to 1, but has diff advantages);
#define MORPHOLOGY_MODE 1

//Comment out features you don't want
#define DEBUG_BOOTSTRAP //get the bootstrap debug (for threshold tuning)
//#define FLOOD_REMOVE_HOLES //fills holes in detected area. Good for large aread detecting (roads) but not small, seperated features (like cones)
#define INVERT_IMAGE_OUPUT //inverts image. The color you want to be detected from your histogram will be white by the algorithm unless inveverted

//###########################################


//VAR Declaration
cv::Mat road_histogram_hue;
cv::Mat nonRoad_histogram_hue;
cv::Mat shadows_histogram_hue;
cv::Mat nonShadows_histogram_hue;
cv::Mat road_mask;
cv::Mat shadows_mask;
cv::Mat compiled_mask;

cv::Mat road_histogram_hue_normalized(361,1,5); //%
cv::Mat nonRoad_histogram_hue_normalized(361,1,5); //%
cv::Mat shadows_histogram_hue_normalized(361,1,5); //%
cv::Mat nonShadows_histogram_hue_normalized(361,1,5); //%

const int hist_bins_hue = 361; //0-360 +1 exclusive
const int hist_bins_saturation = 101; //0-100 +1 exclusive

bool firstRun = true;



//###########################################################
cv::Mat calculateHistogram(cv::Mat image, cv::Mat histogram, int bins){
	bool uniform = true; bool accumulate = false;
	int histSize = bins; //Establish number of BINS

	float range[] = { 0, bins } ; //the upper boundary is exclusive. HUE from 0 - 360
	const float* histRange = { range };
	int channels [] = {0};

	cv::calcHist(&image,1,channels,cv::Mat(),histogram, 1 ,&histSize,&histRange,uniform,accumulate);
	return histogram;
}

cv::Mat calculateHistogramMask(cv::Mat image, cv::Mat histogram, cv::Mat mask, int bins){
	bool uniform = true; bool accumulate = false;//true;//false;
	int histSize = bins; //Establish number of BINS

	float range[] = { 0, bins } ; //the upper boundary is exclusive. HUE from 0 - 360.
	const float* histRange = { range };
	int channels [] = {0};

	cv::calcHist(&image,1,channels,mask,histogram, 1, &histSize,&histRange,uniform,accumulate);
	return histogram;
}



cv::Mat bootstrap(cv::Mat image_hue){
	float threshold = 0.16;//Play with this number 0-1

  const int rectangle_x = 670; //These are magic numbers. If used, they should be played with.
	const int rectangle_y = 760;
	const int rectangle_width = 580;
	const int rectangle_height = 300;

	cv::Mat mask(image_hue.rows, image_hue.cols, CV_8UC1); //mask. Same width/height of input image. Greyscale.
	cv::Mat histogram_hue;

	cv::Mat image_rectangle_hue = image_hue(cv::Rect(rectangle_x,rectangle_y,rectangle_width, rectangle_height));

	histogram_hue = calculateHistogram(image_rectangle_hue,road_histogram_hue,hist_bins_hue);

	//Normalize (sums to 1). Basically gives percentage of pixels of that color (0-1).
	int rectangle_pixels = (rectangle_width * rectangle_height);
	for (int ubin = 0; ubin < hist_bins_hue; ubin++) {
		if(histogram_hue.at<float>(ubin) > 0){
			histogram_hue.at<float>(ubin) /= rectangle_pixels;
		}else{
			histogram_hue.at<float>(ubin) = 0;
		}
	}


	//Mask making
	for(auto i = 0; i < image_hue.rows; i++){
		const unsigned char* row_hue = image_hue.ptr<unsigned char>(i); // HUE
		unsigned char* out_row = mask.ptr<unsigned char>(i); //output

		for(auto j = 0; j < image_hue.cols; j++){
			auto hue = row_hue[j]; //HUE of pixel
			auto histogram_hue_value = histogram_hue.at<float>(hue);

			if(histogram_hue_value >= threshold){
				out_row[j] = 255; //Road. White.
			}else{
				out_row[j] = 0; //nonRoad. Black.
			}

		}

	}

	return mask;
}

//##### #TODO DO NOT FORGET TO RUN road_detector_histogramTrainer.
cv::Mat bootstrapLoadFromStorage(cv::Mat image_hue, float threshold, string fileName, int isShadows){

	cv::Mat mask(image_hue.rows, image_hue.cols, CV_8UC1); //mask. Same width/height of input image. Greyscale.
	cv::Mat histogram_hue;

	//FILE LOADING HISTOGRAM
	// load file
	// per http://stackoverflow.com/questions/10277439/opencv-load-save-histogram-data
	cv::FileStorage fs(fileName + ".yml", cv::FileStorage::READ);
	if (!fs.isOpened()) {ROS_FATAL_STREAM("unable to open file storage!");}
	fs[fileName] >> histogram_hue;
	fs.release();

	//Mask making
	for(auto i = 0; i < image_hue.rows; i++){
		const unsigned char* row_hue = image_hue.ptr<unsigned char>(i); // HUE
		unsigned char* out_row = mask.ptr<unsigned char>(i); //output

		for(auto j = 0; j < image_hue.cols; j++){
			auto hue = row_hue[j]; //HUE of pixel
			auto histogram_hue_value = histogram_hue.at<float>(hue);

			if(histogram_hue_value >= threshold){
				out_row[j] = 255; //Road. White.
			}else{
				out_row[j] = 0; //nonRoad. Black.
			}

		}

	}
	if(isShadows == 0) {
		road_histogram_hue_normalized = histogram_hue;
	}
	if(isShadows == 1) {
		shadows_histogram_hue_normalized = histogram_hue;
	}

	return mask;
}

void trainOnlyOne(cv::Mat &hist_hue_road, cv::Mat mask){
	//Normalize (sums to 1). Basically gives percentage of pixels of that color (0-1).
	int road_pixels = cv::countNonZero(mask); int nonRoad_pixels = cv::countNonZero(~mask);
	for (int ubin = 0; ubin < hist_bins_hue; ubin++) {
		if(hist_hue_road.at<float>(ubin) > 0){
			hist_hue_road.at<float>(ubin) /= road_pixels;
		}
	}

}


void train(cv::Mat &hist_hue_road,cv::Mat &hist_hue_nonRoad, cv::Mat mask){
	//Normalize (sums to 1). Basically gives percentage of pixels of that color (0-1).
	int road_pixels = cv::countNonZero(mask); int nonRoad_pixels = cv::countNonZero(~mask);
	for (int ubin = 0; ubin < hist_bins_hue; ubin++) {
		if(hist_hue_road.at<float>(ubin) > 0){
			hist_hue_road.at<float>(ubin) /= road_pixels;
		}
		if(hist_hue_nonRoad.at<float>(ubin) > 0){
			hist_hue_nonRoad.at<float>(ubin) /= nonRoad_pixels;
		}
	}

}


cv::Mat predict(cv::Mat img_hue, cv::Mat &hist_hue_road, cv::Mat &hist_hue_nonRoad, float threshold){

	//create empty mask			ROS_FATAL_STREAM(road_histogram_hue_normalized.size()); //%
	cv::Mat mask(img_hue.rows, img_hue.cols, CV_8UC1);

	//Mask making based on threshold of road/nonRoad
	for(auto i = 0; i < img_hue.rows; i++){
		const unsigned char* row_hue = img_hue.ptr<unsigned char>(i); // HUE
		unsigned char* out_row = mask.ptr<unsigned char>(i); //output

		for(auto j = 0; j < img_hue.cols; j++){
			auto hue = row_hue[j]; //HUE of pixel
			auto hist_hue_road_val = hist_hue_road.at<float>(hue);
			auto hist_hue_nonRoad_val = hist_hue_nonRoad.at<float>(hue);

			if(hist_hue_nonRoad_val > 0){
				if((hist_hue_road_val / hist_hue_nonRoad_val) >= threshold){
					out_row[j] = 255; //Road. White.
				}else{
					out_row[j] = 0; //nonRoad. Black.
				}
			}else{
					out_row[j] = 0; //nonRoad. Black. //TODO:!!!! WHY THIS? why not check hist_hue_road_val>0 and set white??
			}

		}
	}

	return mask;
}


void morphologyMode1() {
		//############## Do the morphologyEx seperately to allow the Algorithm to learn from more accurate masks. May or may not help.
		cv::imwrite("/home/brian/50Road1.png",road_mask);
		cv::imwrite("/home/brian/51Shadows1.png",shadows_mask);

		auto kernel = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(4,4));
		cv::morphologyEx(shadows_mask,shadows_mask,cv::MORPH_CLOSE,kernel);
		cv::imwrite("/home/brian/52Shadows2_op.png",shadows_mask);

		cv::morphologyEx(road_mask,road_mask,cv::MORPH_CLOSE,kernel);
		cv::imwrite("/home/brian/51Road_op.png",road_mask);
		cv::imwrite("/home/brian/10before.png",road_mask);

		auto kernel1 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(4,4));
		cv::morphologyEx(road_mask,road_mask,cv::MORPH_CLOSE,kernel1);
		cv::imwrite("/home/brian/11close.png",road_mask);

		auto kernel2 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(10,10));
		cv::morphologyEx(road_mask,road_mask,cv::MORPH_OPEN,kernel2);
		cv::imwrite("/home/brian/12open.png",road_mask);
		//Clear out last random black noise. Warning: Loss of detail
		auto kernel3 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(30,30)); //Larger num = no noise but loose exactness. Try 50.
		cv::morphologyEx(road_mask,road_mask,cv::MORPH_CLOSE,kernel3);

//		cv::imwrite("/home/brian/10before.png",compiled_mask);
		//auto kernel1 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(4,4));
		cv::morphologyEx(shadows_mask,shadows_mask,cv::MORPH_CLOSE,kernel1);
//		cv::imwrite("/home/brian/11close.png",compiled_mask);
		//auto kernel2 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(10,10));
		cv::morphologyEx(shadows_mask,shadows_mask,cv::MORPH_OPEN,kernel2);
//		cv::imwrite("/home/brian/12open.png",compiled_mask);

		cv::bitwise_or(road_mask, shadows_mask, compiled_mask);


		//Clear out last random black noise. Warning: Loss of detail
		auto kernel4 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(50,50)); //Larger num = no noise but loose  detail
		cv::morphologyEx(compiled_mask,compiled_mask,cv::MORPH_CLOSE,kernel4);
		cv::imwrite("/home/brian/13close.png",compiled_mask);
}

void morphologyMode2() {
		//Do morphologyEx combined to be faster.
		cv::imwrite("/home/brian/50Road1.png",road_mask);
		cv::imwrite("/home/brian/51Shadows1.png",shadows_mask);

		auto kernel = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(4,4));
		cv::morphologyEx(shadows_mask,shadows_mask,cv::MORPH_CLOSE,kernel);
		cv::imwrite("/home/brian/52Shadows2_op.png",shadows_mask);
		cv::morphologyEx(road_mask,road_mask,cv::MORPH_CLOSE,kernel);
		cv::imwrite("/home/brian/51Road_op.png",road_mask);

		cv::bitwise_or(road_mask, shadows_mask, compiled_mask);
		//morphologyEx Closing operation. #TODO:Keep trying filtering below

		//## may be better solution!! Works 8-28-17 just needs a large kernal the end to destroy last black spots
		cv::imwrite("/home/brian/10before.png",compiled_mask);
		auto kernel1 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(4,4));
		cv::morphologyEx(compiled_mask,compiled_mask,cv::MORPH_CLOSE,kernel1);
		cv::imwrite("/home/brian/11close.png",compiled_mask);
		auto kernel2 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(10,10));
		cv::morphologyEx(compiled_mask,compiled_mask,cv::MORPH_OPEN,kernel2);
		cv::imwrite("/home/brian/12open.png",compiled_mask);

		//Clear out last random black noise. Warning: Loss of detail
		auto kernel3 = cv::getStructuringElement(cv::MORPH_RECT,cv::Size(30,30)); //Larger num = no noise but loose exactness. Try 50.
		cv::morphologyEx(compiled_mask,compiled_mask,cv::MORPH_CLOSE,kernel3);
		cv::imwrite("/home/brian/13close.png",compiled_mask);

}



void img_callback(const sensor_msgs::ImageConstPtr& msg) {
		cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(msg, "bgr8");
		cv::Mat frame = cv_ptr->image;

		cv::Mat frame_hsv;
		cvtColor(frame, frame_hsv, cv::COLOR_BGR2HSV );
		vector<cv::Mat> frame_hsv_planes;
		cv::split(frame_hsv, frame_hsv_planes);

		if(firstRun){
			//Guess of road from stored histogram
			road_mask = bootstrapLoadFromStorage(frame_hsv_planes[0],road_threshold_bootstrap,road_histogram_to_load,0);
			shadows_mask = bootstrapLoadFromStorage(frame_hsv_planes[0],shadows_threshold_bootstrap,shadows_histogram_to_load,1);
			#ifdef DEBUG_BOOTSTRAP
				cv::imwrite("/home/brian/bootstrap_road_00.png",road_mask);
				cv::imwrite("/home/brian/bootstrap_shadows_00.png",shadows_mask);
			#endif

			trainOnlyOne(road_histogram_hue_normalized, road_mask);
			trainOnlyOne(shadows_histogram_hue_normalized,shadows_mask);
			firstRun = false;
		}

		//cutout Horizon/sky #TODO: cut at actual horizon?
		rectangle(road_mask,cv::Point(0,0),cv::Point(road_mask.cols,road_mask.rows / 3), cv::Scalar(0,0,0),CV_FILLED);
		rectangle(shadows_mask,cv::Point(0,0),cv::Point(road_mask.cols,road_mask.rows / 3), cv::Scalar(0,0,0),CV_FILLED);

		road_histogram_hue = calculateHistogramMask(frame_hsv_planes[0],road_histogram_hue,road_mask,hist_bins_hue); //road
		nonRoad_histogram_hue = calculateHistogramMask(frame_hsv_planes[0],nonRoad_histogram_hue,~road_mask,hist_bins_hue); //nonRoad
		train(road_histogram_hue,nonRoad_histogram_hue,road_mask);

		shadows_histogram_hue = calculateHistogramMask(frame_hsv_planes[0],shadows_histogram_hue,shadows_mask,hist_bins_hue); //shadows
		nonShadows_histogram_hue = calculateHistogramMask(frame_hsv_planes[0],nonShadows_histogram_hue,~shadows_mask,hist_bins_hue); //nonShadows
		train(shadows_histogram_hue,nonShadows_histogram_hue,shadows_mask);

		//mix normalize maps. This doesn't make perfect sense to do, but it helps keep mask from inverting if a road is not detected one frame.
		road_histogram_hue_normalized = (road_histogram_hue_normalized * 0.95 + road_histogram_hue * 0.05); /// 2;  //% TODO should weight averages to give previous frames more weight
		nonRoad_histogram_hue_normalized = (nonRoad_histogram_hue_normalized * 0.95 + nonRoad_histogram_hue * 0.05);// / 2; //%

		shadows_histogram_hue_normalized = (shadows_histogram_hue_normalized * 0.99 + shadows_histogram_hue * 0.01);// / 2;  //% TODO should weight averages to give previous frames more weight
		nonShadows_histogram_hue_normalized = (nonShadows_histogram_hue_normalized * 0.99 + nonShadows_histogram_hue * 0.01);// / 2; //%


		road_mask = predict(frame_hsv_planes[0], road_histogram_hue_normalized, nonRoad_histogram_hue_normalized, road_threshold);
		shadows_mask = predict(frame_hsv_planes[0], shadows_histogram_hue_normalized, nonShadows_histogram_hue_normalized, shadows_threshold);
		//#TODO:train-predict loop?

		//cutout Horizon #TODO: cut at actual horizon? Also there is a better way to do this right?
		rectangle(road_mask,cv::Point(0,0),cv::Point(road_mask.cols,road_mask.rows / 3), cv::Scalar(0,0,0),CV_FILLED);
		rectangle(shadows_mask,cv::Point(0,0),cv::Point(road_mask.cols,road_mask.rows / 3), cv::Scalar(0,0,0),CV_FILLED);

		switch (MORPHOLOGY_MODE) {
			case 0: break;
			case 1: morphologyMode1();
							break;
			case 2: morphologyMode2();
							break;
		}


		#ifdef FLOOD_REMOVE_HOLES
		// Remove holes by flood fill method (@reference https://www.learnopencv.com/filling-holes-in-an-image-using-opencv-python-c/)
			cv::Mat compiled_mask_invert;
			cv::bitwise_not(compiled_mask,compiled_mask_invert);
			cv::floodFill(compiled_mask_invert, cv::Point(0,0), cv::Scalar(0), 0, cv::Scalar(), cv::Scalar(), cv::FLOODFILL_FIXED_RANGE);
			cv::bitwise_or(compiled_mask,compiled_mask_invert,compiled_mask);
		#endif

		#ifdef INVERT_IMAGE_OUPUT
			cv::bitwise_not(compiled_mask,compiled_mask);
		#endif


		/* THE PLAN:
		bootstrap mask (guess from rectangle) (or guess from histogram from file)
		->
		calculate Histograms for road and non road;
		train those histograms (make them each add to 1)
		average noramalized histograms ?
		predict(create a new mask based on threshold compare of road and nonRoad)
		(repeat at -> x times)
		filter blur (something that checks if pixels around it are white, then makes it white or black based on that)
		*/

    //publish image
    sensor_msgs::Image outmsg;
    cv_ptr->image = compiled_mask;
    cv_ptr->encoding = "mono8";
    cv_ptr->toImageMsg(outmsg);

    pub.publish(outmsg);
}



int main(int argc, char** argv) {
	ros::init(argc, argv, "road_detector");

	ros::NodeHandle nh;
  pub = nh.advertise<sensor_msgs::Image>("/road_detector", 1); //test publish of image
	auto img_sub = nh.subscribe("/camera/image_rect", 1, img_callback);

	ros::spin();
	return 0;

}
