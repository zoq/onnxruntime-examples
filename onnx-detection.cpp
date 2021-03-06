#include <cpu_provider_factory.h>
// #include <cuda_provider_factory.h>

#include <onnxruntime_cxx_api.h>

#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int YOLO_GRID_X = 13;
int YOLO_GRID_Y = 13;
int YOLO_NUM_BB = 5;

double anchors[] =
{
  1.08, 1.19,
  3.42, 4.41,
  6.63, 11.38,
  9.42, 5.11,
  16.62, 10.52
};

class Image
{
  public:
    Image(int w, int h, int c)
    {
      img_buffer.reserve(w * h * c);
      apl_buffer.reserve(w * h * c);
      img_w = w;
      img_h = h;
      img_c = c;
    }

    std::vector<unsigned char> img_buffer;

    unsigned char at(int a)
    {
      return img_buffer[a];
    }

    void set(int a, unsigned char val)
    {
      img_buffer[a] = val;
    }

  private:
    std::vector<unsigned char> apl_buffer;
    int img_h;
    int img_w;
    int img_c;
};

int offset_(int b, int y, int x, const size_t classes)
{
  return b * (classes + 5) * YOLO_GRID_X * YOLO_GRID_Y + y * YOLO_GRID_X + x;
}

int offset(int o, int channel)
{
  return o + channel * YOLO_GRID_X * YOLO_GRID_Y;
}

double Sigmoid(double x)
{
  return 1.0 / (1.0 + exp(-x));
}

void Softmax(float val[], const size_t classes)
{
  float max = -INT_MAX;
  float sum = 0;

  for (size_t i = 0; i < classes; ++i)
    max = std::max(max, val[i]);

  for (size_t i = 0; i < classes; ++i)
  {
    val[i] = (float) exp(val[i] - max);
    sum += val[i];
  }

  for (size_t i = 0; i < classes; ++i)
    val[i] = val[i] / sum;
}

typedef struct
{
  float x;
  float y;
  float w;
  float h;
} Box;

typedef struct detection
{
  Box bbox;
  float conf;
  int c;
  float prob;
} detection;

typedef struct
{
 float dx;
 float dy;
 float dw;
 float dh;
} Dbox;

Box FloatToBox(const float fx, const float fy, const float fw, const float fh)
{
  Box b;
  b.x = fx;
  b.y = fy;
  b.w = fw;
  b.h = fh;

  return b;
}

float Overlap(float x1, float w1, float x2, float w2)
{
  float l1 = x1 - w1 / 2;
  float l2 = x2 - w2 / 2;
  float left = l1 > l2 ? l1 : l2;

  float r1 = x1 + w1 / 2;
  float r2 = x2 + w2 / 2;
  float right = r1 < r2 ? r1 : r2;

  return right - left;
}

float BoxIntersection(const Box& a, const Box& b)
{
  float w = Overlap(a.x, a.w, b.x, b.w);
  float h = Overlap(a.y, a.h, b.y, b.h);

  if(w < 0 || h < 0)
    return 0;

  return w * h;
}

float BoxUnion(const Box& a, const Box& b)
{
  return a.w * a.h + b.w * b.h - BoxIntersection(a, b);
}

float BoxIOU(const Box& a, const Box& b)
{
  return BoxIntersection(a, b) / BoxUnion(a, b);
}

void FilterBoxesNMS(std::vector<detection>& det, int nBoxes, float th_nms)
{
  int count = nBoxes;
  for (size_t i = 0;i < count; ++i)
  {
    Box a = det[i].bbox;
    for (size_t j = 0; j < count; ++j)
    {
      if (i == j) continue;
      if (det[i].c != det[j].c) continue;

      Box b = det[j].bbox;
      float b_intersection = BoxIntersection(a, b);
      if (BoxIOU(a, b) > th_nms ||
          b_intersection >= a.h * a.w - 1 ||
          b_intersection >= b.h * b.w - 1)
      {
        if (det[i].prob > det[j].prob)
        {
          det[j].prob = 0;
        }
        else
        {
          det[i].prob = 0;
        }
      }
    }
  }
}

template <typename T>
T vectorProduct(const std::vector<T>& v)
{
  return accumulate(v.begin(), v.end(), 1, std::multiplies<T>());
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& v)
{
  os << "[";
  for (int i = 0; i < v.size(); ++i)
  {
      os << v[i];
      if (i != v.size() - 1)
      {
          os << ", ";
      }
  }
  os << "]";

  return os;
}

std::vector<std::string> ReadLabels(const std::string& labelsFile)
{
    std::vector<std::string> labels;
    std::string line;
    std::ifstream fp(labelsFile);
    while (std::getline(fp, line))
        labels.push_back(line);

    return labels;
}

int main(int argc, char* argv[])
{
  bool useCUDA{true};
  const char* useCUDAFlag = "--use_cuda";
  const char* useCPUFlag = "--use_cpu";

  if (argc == 1)
  {
      useCUDA = false;
  }
  else if ((argc == 2) && (strcmp(argv[1], useCUDAFlag) == 0))
  {
      useCUDA = true;
  }
  else if ((argc == 2) && (strcmp(argv[1], useCPUFlag) == 0))
  {
      useCUDA = false;
  }
  else if ((argc == 2) && (strcmp(argv[1], useCUDAFlag) != 0))
  {
      useCUDA = false;
  }
  else
  {
      throw std::runtime_error{"Too many arguments."};
  }

  if (useCUDA)
  {
      std::cout << "Inference Execution Provider: CUDA" << std::endl;
  }
  else
  {
      std::cout << "Inference Execution Provider: CPU" << std::endl;
  }

  const double confidenceThreshold = 0.5;
  const double maskThreshold = 0.6;

  const std::string modelFilepath = "data/models/Model.onnx";
  const std::string imageFilepath = "data/images/test.jpg";
  const std::string labelFilepath = "data/labels/VOC_pascal_classes.txt";
  const std::string instanceName = "image-classification-inference";

  const std::vector<std::string> labels = ReadLabels(labelFilepath);

  int imageWidth, imageHeight, imageChannels;
  stbi_uc * img_data = stbi_load(imageFilepath.c_str(), &imageWidth,
      &imageHeight, &imageChannels, STBI_default);

  if (imageWidth != 416 || imageHeight != 416 || imageChannels != 3)
  {
    printf("Image size does't match with 416 x 416 x 3");
    return 1;
  }

  struct Pixel { unsigned char RGBA[3]; };
  const Pixel* imgPixels((const Pixel*)img_data);

  Ort::Env env(OrtLoggingLevel::ORT_LOGGING_LEVEL_WARNING,
      instanceName.c_str());
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1);

  // Use CUDA backend.
  if (useCUDA)
  {
    /* OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_CUDA( */
    /*     sessionOptions, 0); */
  }

  // Sets graph optimization level:
  // ORT_DISABLE_ALL - Disable all optimizations.
  // ORT_ENABLE_BASIC - Enable basic optimizations (redundant node removals).
  // ORT_ENABLE_EXTENDED - To enable extended optimizations (redundant node
  //     removals + node fusions).
  // ORT_ENABLE_ALL - Enable all possible optimizations.
  sessionOptions.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

  Ort::Session session(env, modelFilepath.c_str(), sessionOptions);

  Ort::AllocatorWithDefaultOptions allocator;

  size_t numInputNodes = session.GetInputCount();
  std::cout << "Number of Input Nodes: " << numInputNodes << std::endl;

  size_t numOutputNodes = session.GetOutputCount();
  std::cout << "Number of Output Nodes: " << numOutputNodes << std::endl;

  const char* inputName = session.GetInputName(0, allocator);
  std::cout << "Input Name: " << inputName << std::endl;

  Ort::TypeInfo inputTypeInfo = session.GetInputTypeInfo(0);
  std::vector<int64_t> inputDims =
      inputTypeInfo.GetTensorTypeAndShapeInfo().GetShape();

  // Change the first dimension from -1 to 1, necessary for Tiny YOLO v2.
  if (inputDims[0] < 0)
    inputDims[0] = 1;
  std::cout << "Input Dimensions: " << inputDims << std::endl;

  std::string outputName = session.GetOutputName(0, allocator);
  std::cout << "Output Name: " << outputName << std::endl;

  Ort::TypeInfo outputTypeInfo = session.GetOutputTypeInfo(0);
  auto outputTensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();

  std::vector<int64_t> outputDims = outputTensorInfo.GetShape();
  // Change the first dimension from -1 to 1, necessary for Tiny YOLO v2.
  if (outputDims[0] < 0)
    outputDims[0] = 1;
  std::cout << "Output Dimensions: " << outputDims << std::endl;

  // The model expects the image to be of size 416 x 416 x 3.
  const size_t inputTensorSize = imageWidth * imageHeight * imageChannels;
  std::vector<float> inputTensorValues(inputTensorSize);

  // Transpose image.
  Image *img = new Image(imageWidth, imageHeight, 3);
  size_t shift = 0;
  for (size_t c = 0; c < 3; ++c)
  {
    for (size_t y = 0; y < imageHeight; ++y)
    {
      for (size_t x = 0; x < imageWidth; ++x, ++shift)
      {
        const int val(imgPixels[y * imageWidth + x].RGBA[c]);
        img->set((y * imageWidth + x) * 3 + c, val);
        inputTensorValues[shift] = val;
      }
    }
  }

  // Has to match with the model output type.
  const size_t outputTensorSize = vectorProduct(outputDims);
  std::vector<float> outputTensorValues(outputTensorSize);

  std::vector<const char*> inputNames{inputName};
  std::vector<const char*> outputNames{outputName.c_str()};

  std::vector<Ort::Value> inputTensors;
  std::vector<Ort::Value> outputTensors;

  Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
      OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

  inputTensors.push_back(Ort::Value::CreateTensor<float>(
      memoryInfo, inputTensorValues.data(), inputTensorSize, inputDims.data(),
      inputDims.size()));

  outputTensors.push_back(Ort::Value::CreateTensor<float>(
      memoryInfo, outputTensorValues.data(), outputTensorSize,
      outputDims.data(), outputDims.size()));

  session.Run(Ort::RunOptions{nullptr}, inputNames.data(),
              inputTensors.data(), 1, outputNames.data(),
              outputTensors.data(), 1);

  size_t numClasses = labels.size();

  size_t count = 0;
  std::vector<detection> det;
  for (size_t b = 0; b < YOLO_NUM_BB; ++b)
  {
    for (size_t y = 0; y < YOLO_GRID_Y; ++y)
    {
      for (size_t x = 0; x < YOLO_GRID_X; ++x)
      {
        int offs = offset_(b, y, x, numClasses);
        float tc = outputTensorValues[offset(offs, 4)];
        float conf = Sigmoid(tc);

        if (conf > confidenceThreshold)
        {
          float tx = outputTensorValues[offs];
          float ty = outputTensorValues[offset(offs, 1)];
          float tw = outputTensorValues[offset(offs, 2)];
          float th = outputTensorValues[offset(offs, 3)];

          float xPos = ((float) x + Sigmoid(tx)) * 32;
          float yPos = ((float) y + Sigmoid(ty)) * 32;
          float wBox = (float) exp(tw) * anchors[2 * b + 0] * 32;
          float hBox = (float) exp(th) * anchors[2 * b + 1] * 32;

          Box bb = FloatToBox(xPos, yPos, wBox, hBox);

          float classes[numClasses];
          for (int c = 0; c < numClasses; ++c)
            classes[c] = outputTensorValues[offset(offs, 5 + c)];

          Softmax(classes, numClasses);
          float maxPD = 0;
          int detected = -1;

          for (int c = 0; c < numClasses; ++c)
          {
            if (classes[c] > maxPD)
            {
              detected = c;
              maxPD = classes[c];
            }
          }

          float score = maxPD * conf;
          if (score > maskThreshold)
          {
            detection d = {bb, conf , detected, maxPD};
            det.push_back(d);
            count++;
          }
        }
      }
    }
  }

  // NMS filter.
  FilterBoxesNMS(det, count, 0.2);

  int j = 0;
  for (int i = 0; i < count; ++i)
  {
    if (det[i].prob == 0) continue;
    j++;

    std::cout << std::endl;
    std::cout << "Label: " << labels[det[i].c] << std::endl;
    std::cout << "Bounding Box (X, Y, W, H): (" << det[i].bbox.x << ", "
        << det[i].bbox.y << ", " << det[i].bbox.w << ", " << det[i].bbox.h << ")"
        << std::endl;
    std::cout << "Confidence (IoU): " << det[i].conf * 100 << "%" << std::endl;
    std::cout << "Probability: " << det[i].prob * 100 << "%" << std::endl;
    std::cout << "Score: " << det[i].prob * det[i].conf * 100 << "%"
        << std::endl;

  }

  return 0;
}
