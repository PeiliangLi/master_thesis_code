#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/bbtxt_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

// The maximum number of bounding boxes (annotations) in one image - we set the label blob shape according
// to this number
#define MAX_NUM_BBS_PER_IMAGE 20


namespace caffe {

namespace {


    /**
     * @brief Computes the number of bounding boxes in the annotation
     * @param labels Annotation of one image (dimensions 1 x MAX_NUM_BBS_PER_IMAGE x 5)
     * @return Number of bounding boxes in this label
     */
    template <typename Dtype>
    int numBBs (const Blob<Dtype> &labels)
    {
        for (int i = 0; i < labels.shape(1); ++i)
        {
            // Data are stored like this [label, xmin, ymin, xmax, ymax]
            const Dtype *data = labels.cpu_data() + labels.offset(0, i);
            // If the label is -1, there are no more bounding boxes
            if (data[0] == Dtype(-1.0f)) return i;
        }

        return labels.shape(1);
    }

}

template <typename Dtype>
BBTXTDataLayer<Dtype>::BBTXTDataLayer (const LayerParameter &param)
    : BasePrefetchingDataLayer<Dtype>(param)
{
}


template <typename Dtype>
BBTXTDataLayer<Dtype>::~BBTXTDataLayer<Dtype> ()
{
    this->StopInternalThread();
}


template <typename Dtype>
void BBTXTDataLayer<Dtype>::DataLayerSetUp (const vector<Blob<Dtype>*> &bottom,
                                            const vector<Blob<Dtype>*> &top)
{
    CHECK(this->layer_param_.has_bbtxt_param()) << "BBTXTParam is mandatory!";
    CHECK(this->layer_param_.bbtxt_param().has_height()) << "Height must be set!";
    CHECK(this->layer_param_.bbtxt_param().has_width()) << "Width must be set!";
    CHECK(this->layer_param_.bbtxt_param().has_reference_size()) << "Reference size must be set!";

    const int height     = this->layer_param_.bbtxt_param().height();
    const int width      = this->layer_param_.bbtxt_param().width();
    const int batch_size = this->layer_param_.image_data_param().batch_size();

    this->_rng.reset(new Caffe::RNG(caffe_rng_rand()));

    // Load the BBTXT file with 2D bounding box annotations
    this->_loadBBTXTFile();
    this->_i_global = 0;

    CHECK(!this->_images.empty()) << "The given BBTXT file is empty!";
    LOG(INFO) << "There are " << this->_images.size() << " images in the dataset set.";

    if (this->layer_param_.image_data_param().shuffle())
    {
        // Initialize the random number generator for shuffling and shuffle the images
        this->_shuffleImages();
    }


    // This is the shape of the input blob
    std::vector<int> top_shape = {1, 3, height, width};
    this->transformed_data_.Reshape(top_shape);  // For prefetching

    top_shape[0] = batch_size;
    top[0]->Reshape(top_shape);

    // Label blob
    std::vector<int> label_shape = {1, MAX_NUM_BBS_PER_IMAGE, 5};
    this->transformed_label_.Reshape(label_shape);  // For prefetching

    label_shape[0] = batch_size;
    top[1]->Reshape(label_shape);


    // Initialize prefetching
    // We also have to reshape the prefetching blobs to the correct batch size
    for (int i = 0; i < this->prefetch_.size(); ++i)
    {
        this->prefetch_[i]->data_.Reshape(top_shape);
        this->prefetch_[i]->label_.Reshape(label_shape);
    }
}


template <typename Dtype>
void BBTXTDataLayer<Dtype>::load_batch (Batch<Dtype> *batch)
{
    // This function is called on a prefetch thread

    CHECK(batch->data_.count());
    CHECK(this->transformed_data_.count());

    const int batch_size = this->layer_param_.image_data_param().batch_size();

    Dtype* prefetch_data  = batch->data_.mutable_cpu_data();
    Dtype* prefetch_label = batch->label_.mutable_cpu_data();

    for (int b = 0; b < batch_size; ++b)
    {
        // get a blob
        cv::Mat cv_img = cv::imread(this->_images[this->_i_global].first, CV_LOAD_IMAGE_COLOR);
        CHECK(cv_img.data) << "Could not open " << this->_images[this->_i_global].first;

        // Prepare the blob for the annotation (bounding boxes)
        int offset_label = batch->label_.offset(b);
        this->transformed_label_.set_cpu_data(prefetch_label + offset_label);
        // Copy the annotation - we really have to copy it because it will be altered during image
        // transformations like cropping or scaling
        std::shared_ptr<Blob<Dtype>> plabel = this->_images[this->_i_global].second;
        caffe_copy(plabel->count(), plabel->cpu_data(), this->transformed_label_.mutable_cpu_data());

        // Prepare the blob for the current image
        int offset_image = batch->data_.offset(b);
        this->transformed_data_.set_cpu_data(prefetch_data + offset_image);

        // Apply transformations (mirror, crop...) to the image and resize the image to the input blob shape
        // of the network - setting the transformed_data_ and transformed_label_ sets it directly in the batch
        this->_transformImage(cv_img, this->transformed_data_, this->transformed_label_);

        // Move index to the next image
        this->_i_global++;
        if (this->_i_global >= this->_images.size())
        {
            // Restart the counter from the begining
            if (this->layer_param_.image_data_param().shuffle()) this->_shuffleImages();
            this->_i_global = 0;
        }
    }
}



// -----------------------------------------  PROTECTED METHODS  ----------------------------------------- //

template <typename Dtype>
void BBTXTDataLayer<Dtype>::_loadBBTXTFile ()
{
    const std::string& source = this->layer_param_.image_data_param().source();

    std::ifstream infile(source.c_str(), std::ios::in);
    CHECK(infile.is_open()) << "BBTXT file '" << source << "' could not be opened!";

    std::string line;
    std::vector<std::string> data;
    std::string current_filename = "";
    int i = 0;

    // Read the whole file and create entries in the _images for all images
    while (std::getline(infile, line))
    {
        // Split the line - entries separated by space [filename label confidence xmin ymin xmax ymax]
        boost::split(data, line, boost::is_any_of(" "));
        CHECK_EQ(data.size(), 7) << "Line '" << line << "' corrupted!";

        if (current_filename != data[0])
        {
            // This is a label to a new image
            if (this->_images.size() > 0 && i < MAX_NUM_BBS_PER_IMAGE)
            {
                // Finalize the last annotation - we put -1 as next bounding box label to signalize
                // the end - this is because each image can have a different number of bounding boxes
                int offset = this->_images.back().second->offset(i);
                Dtype* bb_position = this->_images.back().second->mutable_cpu_data() + offset;
                bb_position[0] = Dtype(-1.0f);
            }

            CHECK(boost::filesystem::exists(data[0])) << "File '" << data[0] << "' not found!";

            // Create new image entry
            this->_images.push_back(std::make_pair(data[0],
                                        std::make_shared<Blob<Dtype>>(MAX_NUM_BBS_PER_IMAGE, 5, 1, 1)));
            i = 0;
            current_filename = data[0];
        }

        // Write the bounding box info into the blob
        if (i < MAX_NUM_BBS_PER_IMAGE)
        {
            int offset = this->_images.back().second->offset(i);
            Dtype* bb_position = this->_images.back().second->mutable_cpu_data() + offset;
            bb_position[0] = Dtype(std::stof(data[1])); // label
            bb_position[1] = Dtype(std::stof(data[3])); // xmin
            bb_position[2] = Dtype(std::stof(data[4])); // ymin
            bb_position[3] = Dtype(std::stof(data[5])); // xmax
            bb_position[4] = Dtype(std::stof(data[6])); // ymax
            i++;
        }
        else
        {
            LOG(WARNING) << "Skipping bb - max number of bounding boxes per image reached.";
        }
    }
}


template <typename Dtype>
void BBTXTDataLayer<Dtype>::_shuffleImages ()
{
    caffe::rng_t* prefetch_rng = static_cast<caffe::rng_t*>(_rng->generator());
    shuffle(this->_images.begin(), this->_images.end(), prefetch_rng);
}


template <typename Dtype>
void BBTXTDataLayer<Dtype>::_transformImage (const cv::Mat &cv_img, Blob<Dtype> &transformed_image,
                                             Blob<Dtype> &transformed_label)
{
    static int imi = 0;
    CHECK_EQ(cv_img.channels(), 3) << "Image must have 3 color channels";

    // Input dimensions of the network
    const int height          = this->layer_param_.bbtxt_param().height();
    const int width           = this->layer_param_.bbtxt_param().width();
    // This is the size of the bounding box that is detected by this network
    const int reference_size  = this->layer_param_.bbtxt_param().reference_size();

    caffe::rng_t* rng = static_cast<caffe::rng_t*>(this->_rng->generator());


    // We select a bounding box from the image and then make a crop such that the bounding box is inside
    // of it and it has the reference size
    cv::Mat cv_img_cropped;
    if (transformed_label.cpu_data()[0] == Dtype(-1.0f))
    {
        // The label of the first bounding box is -1, that means that this image contains no bounding boxes
        cv::resize(cv_img, cv_img_cropped, cv::Size(width, height));
    }
    else
    {
        std::cout << "Doing this.................." << std::endl;
        // There are bounding boxes - lets select one
        const int num_bbs = numBBs(transformed_label);
        boost::random::uniform_int_distribution<> dist(0, num_bbs-1);
        const int bb_id = dist(*rng);  // Random bounding box id

        // Get dimensions of the bounding box - format [label, xmin, ymin, xmax, ymax]
        const Dtype * bb_data = transformed_label.cpu_data() + transformed_label.offset(0, bb_id);
        const Dtype x = bb_data[1];
        const Dtype y = bb_data[2];
        const Dtype w = bb_data[3] - bb_data[1];
        const Dtype h = bb_data[4] - bb_data[2];

        const Dtype size      = std::max(w, h);
        const int crop_width  = double(width) / reference_size * size;
        const int crop_height = double(height) / reference_size * size;

        // Select a random position of the crop, but it has to fully contain the bounding box
        boost::random::uniform_int_distribution<> distx(x+w-crop_width, x);
        boost::random::uniform_int_distribution<> disty(y+h-crop_height, y);
        const int crop_x = distx(*rng);
        const int crop_y = disty(*rng);

        // Now if the crop spans outside the image we have to pad the image
        int border_top = 0; int border_bottom = 0; int border_left = 0; int border_right = 0;
        if (crop_x < 0) border_left = -crop_x;
        if (crop_y < 0) border_top  = -crop_y;
        if (crop_x+crop_width > cv_img.cols)  border_right  = crop_x+crop_width - cv_img.cols;
        if (crop_y+crop_height > cv_img.rows) border_bottom = crop_y+crop_height - cv_img.rows;

        cv::Mat cv_img_padded;
        cv::copyMakeBorder(cv_img, cv_img_padded, border_top, border_bottom, border_left, border_right,
                           cv::BORDER_REPLICATE);

        // Crop
        cv_img_cropped = cv_img_padded(cv::Rect(crop_x+border_left, crop_y+border_top, crop_width, crop_height));

        // Resize
        cv::resize(cv_img_cropped, cv_img_cropped, cv::Size(width, height));


        // Update the bounding box coordinates - we need to update all annotations
        Dtype x_scaling = float(width) / crop_width;
        Dtype y_scaling = float(height) / crop_height;
        for (int b = 0; b < num_bbs; ++b)
        {
            // Data are stored like this [label, xmin, ymin, xmax, ymax]
            Dtype *data = transformed_label.mutable_cpu_data() + transformed_label.offset(0, b);
            // Align with x, y of the crop
            data[1] -= crop_x;
            data[2] -= crop_y;
            data[3] -= crop_x;
            data[4] -= crop_y;
            // Now the scaling
            data[1] *= x_scaling;
            data[2] *= y_scaling;
            data[3] *= x_scaling;
            data[4] *= y_scaling;

//            cv::rectangle(cv_img_cropped, cv::Rect(data[1], data[2], data[3]-data[1], data[4]-data[2]), cv::Scalar(0,0,255), 2);
        }
//        cv::imwrite("cropped" + std::to_string(imi++) + ".png", cv_img_cropped);

        // Mirror
    }


    CHECK(cv_img_cropped.data) << "Something went wrong with cropping!";
    CHECK_EQ(cv_img_cropped.rows, transformed_image.shape(2)) << "Wrong crop height! Does not match network!";
    CHECK_EQ(cv_img_cropped.cols, transformed_image.shape(3)) << "Wrong crop width! Does not match network!";

    // Normalize to 0 mean and unit variance and copy the image to the transformed_image
    // + apply exposure, noise, hue, saturation, ...
    Dtype* transformed_data = transformed_image.mutable_cpu_data();
    for (int i = 0; i < height; ++i)
    {
        const uchar* ptr = cv_img_cropped.ptr<uchar>(i);
        int img_index = 0;  // Index in the cv_img_cropped
        for (int j = 0; j < width; ++j)
        {
            for (int c = 0; c < 3; ++c)
            {
                const int top_index = (c * height + i) * width + j;
                // Zero mean and unit variance
                transformed_data[top_index] = (ptr[img_index++] - Dtype(128.0f)) / Dtype(128.0f);
            }
        }
    }
}



// ----------------------------------------  LAYER INSTANTIATION  ---------------------------------------- //

INSTANTIATE_CLASS(BBTXTDataLayer);
REGISTER_LAYER_CLASS(BBTXTData);


}  // namespace caffe
#endif  // USE_OPENCV
