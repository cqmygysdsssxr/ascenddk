/**
 * ============================================================================
 *
 * Copyright (C) 2018, Hisilicon Technologies Co., Ltd. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1 Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   2 Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 *   3 Neither the names of the copyright holders nor the names of the
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * ============================================================================
 */

#include "video_decode.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/prctl.h>

#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include "hiaiengine/log.h"
#include "hiaiengine/data_type_reg.h"

using namespace std;

namespace {
// The width of image in vpc interface need 128-byte alignment
const int kVpcWidthAlign = 128;

// The height of image in vpc interface need 16-byte alignment
const int kVpcHeightAlign = 16;

// standard: 4096 * 4096 * 4 = 67108864 (64M)
const int kAllowedMaxImageMemory = 67108864;

const int kWait10Milliseconds = 10000; // wait 10ms

const int kWait100Milliseconds = 100000; // wait 100ms

const int kKeyFrameInterval = 5; // key fram interval

const int kImageDataQueueSize = 10; // the queue default size

const int kMaxRetryNumber = 100; // the maximum number of retry

const int kCompareEqual = 0; // string compare equal

const int kNoFlag = 0; // no flag

const int kHandleSuccessful = 0; // the process handled successfully

const string kStrChannelId1 = "channel1"; // channle id 1 string

const string kStrChannelId2 = "channel2";  // channle id 2 string

const string kVideoTypeH264 = "h264"; // video type h264

const string kVideoTypeH265 = "h265"; // video type h265

const string kNeedRemoveStr = " \r\n\t"; // the string need remove

const string kVideoImageParaType = "VideoImageParaT"; // video image para type

const int kIntChannelId1 = 1; // channel id 1 integer

const int kIntChannelId2 = 2; // channel id 2 integer

const int kVideoFormatLength = 5; // video format string length

const int kInvalidVideoIndex = -1; // invalid video index

const string kImageFormatNv12 = "nv12"; // image format nv12

const string kRtspTransport = "rtsp_transport"; // rtsp transport

const string kUdp = "udp"; // video format udp

const string kBufferSize = "buffer_size"; // buffer size string

const string kMaxBufferSize = "104857600"; // maximum buffer size:100MB

const string kMaxDelayStr = "max_delay"; // maximum delay string

const string kMaxDelayValue = "100000000"; // maximum delay time:100s

const string kTimeoutStr = "stimeout"; // timeout string

const string kTimeoutValue = "5000000"; // timeout:5s

const string kPktSize = "pkt_size"; // ffmpeg pakect size string

const string kPktSizeValue = "10485760"; // ffmpeg packet size value:10MB

const string kReorderQueueSize = "reorder_queue_size"; // reorder queue size

const string kReorderQueueSizeValue = "0"; // reorder queue size value

const string kThreadNameHead = "handle_"; // thread name head string

const int kErrorBufferSize = 1024; // buffer size for error info

const int kThreadNameLength = 32; // thread name string length

const string kRegexSpace = "^[ ]*$"; // regex for check string is empty

// regex for verify .mp4 file name
const string kRegexMp4File = "^/((?!\\.\\.).)*\\.(mp4)$";

// regex for verify RTSP rtsp://ip:port/channelname
const string kRegexRtsp =
    "^rtsp://(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|[0-9])\\."
        "(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|\\d)\\."
        "(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|\\d)\\."
        "(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|\\d)"
        ":([1-9]|[1-9]\\d|[1-9]\\d{2}|[1-9]\\d{3}|[1-5]\\d{4}|"
        "6[0-4]\\d{3}|65[0-4]\\d{2}|655[0-2]\\d|6553[0-5])/"
        "(.{1,100})$";

uint32_t frame_id_1 = 0; // frame id used for channle1

uint32_t frame_id_2 = 0; // frame id used for channle2

// the queue record image data from channel1
ThreadSafeQueue<shared_ptr<VideoImageParaT>> channel1_queue(
    kImageDataQueueSize);

// the queue record image data from channel2
ThreadSafeQueue<shared_ptr<VideoImageParaT>> channel2_queue(
    kImageDataQueueSize);
}

HIAI_REGISTER_DATA_TYPE("VideoImageParaT", VideoImageParaT);

bool IsKeyFrame(uint32_t frame_id) {
  // the 1, 6, 11, 16... frame is key frame
  if (frame_id % kKeyFrameInterval == 1) {
    return true;
  }

  return false;
}

VideoDecode::VideoDecode() {
  channel1_ = ""; // initialize channel1 to empty string
  channel2_ = ""; // initialize channel2 to empty string
}

VideoDecode::~VideoDecode() {
}

void VideoDecode::SendFinishedData() {
  VideoImageInfoT video_image_info;
  video_image_info.is_finished = true;

  hiai::ImageData<unsigned char> image_data;
  shared_ptr<VideoImageParaT> video_image_para = make_shared<VideoImageParaT>();
  video_image_para->img = image_data;
  video_image_para->video_image_info = video_image_info;

  HIAI_StatusT hiai_ret = HIAI_OK;

  // send finished data to next engine, use output port:0
  do {
    hiai_ret = SendData(0, kVideoImageParaType,
                        static_pointer_cast<void>(video_image_para));
    if (hiai_ret == HIAI_QUEUE_FULL) { // check hiai queue is full
      HIAI_ENGINE_LOG("Queue full when send finished data, sleep 10ms");
      usleep(kWait10Milliseconds);  // sleep 10 ms
    }
  } while (hiai_ret == HIAI_QUEUE_FULL); // loop when hiai queue is full

  if (hiai_ret != HIAI_OK) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Send finished data failed! error code: %d", hiai_ret);
  }
}

uint32_t GetFrameId(const string &channel_id) {
  if (channel_id == kStrChannelId1) { // check input channel id is channel1
    frame_id_1++;
    return frame_id_1;
  } else { // the input channel id is channel2
    frame_id_2++;
    return frame_id_2;
  }
}

void AddImage2QueueByChannel(
    const shared_ptr<VideoImageParaT>& video_image_para,
    ThreadSafeQueue<shared_ptr<VideoImageParaT>> &current_queue) {
  bool add_image_success = false;
  int count = 0; // count retry number

  // add image data to queue, max retry time: 100
  while (count < kMaxRetryNumber) {
    count++;

    if (current_queue.Push(video_image_para)) { // push image data to queue
      add_image_success = true;
      break;
    } else { // queue is full, sleep 100 ms
      usleep(kWait100Milliseconds);
    }
  }

  if (!add_image_success) { // fail to send image data
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to add image data to queue, channel_id:%s, "
                    "channel_name:%s, frame_id:%d",
                    video_image_para->video_image_info.channel_id.c_str(),
                    video_image_para->video_image_info.channel_name.c_str(),
                    video_image_para->video_image_info.frame_id);
  }
}

void SendKeyFrameData(uint8_t* image_data_buffer, uint32_t image_data_size,
                      void* hiai_data, FRAME* frame) {
  string channel_name = ((YuvImageFrameInfo*) (hiai_data))->channel_name;
  string channel_id = ((YuvImageFrameInfo*) (hiai_data))->channel_id;
  uint32_t frame_id = GetFrameId(channel_id);

  // only send key frame to next engine, key frame id: 1,6,11,16...
  if (!IsKeyFrame(frame_id)) {
    delete[] image_data_buffer;
    return;
  }

  HIAI_ENGINE_LOG("Get key frame, frame id:%d, channel_id:%s, channel_name:%s,"
                  " frame->realWidth:%d, frame->realHeight:%d",
                  frame_id, channel_id.c_str(), channel_name.c_str(),
                  frame->realWidth, frame->realHeight);

  //send yuv420sp data
  VideoImageInfoT i_video_image_info;
  i_video_image_info.channel_id = channel_id;
  i_video_image_info.channel_name = channel_name;
  i_video_image_info.frame_id = frame_id;
  i_video_image_info.is_finished = false;

  hiai::ImageData<unsigned char> image_data;
  image_data.width = frame->realWidth;
  image_data.height = frame->realHeight;
  image_data.format = IMAGEFORMAT::YUV420SP;
  image_data.size = image_data_size;

  image_data.data.reset(image_data_buffer, default_delete<unsigned char[]>());
  shared_ptr<VideoImageParaT> video_image_para = make_shared<VideoImageParaT>();
  video_image_para->img = image_data;
  video_image_para->video_image_info = i_video_image_info;

  if (channel_id == kStrChannelId1) { // add image data to channel1 queue
    AddImage2QueueByChannel(video_image_para, channel1_queue);
  } else { // add image data to channel2 queue
    AddImage2QueueByChannel(video_image_para, channel2_queue);
  }
}

void CallVpcGetYuvImage(FRAME* frame, void* hiai_data) {
  if (frame == nullptr || hiai_data == nullptr) { // check input parameters
    HIAI_ENGINE_LOG(
        HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
        "The input data for function:CallVpcGetYuvImage is nullptr!");
    return;
  }

  IDVPPAPI* dvpp_api = nullptr;
  CreateDvppApi(dvpp_api);

  if (dvpp_api == nullptr) { // check create dvpp api result
    HIAI_ENGINE_LOG(
        HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
        "Fail to call CreateDvppApi for vpc, the result is nullptr!");
    return;
  }

  // constructing input image configuration
  shared_ptr<VpcUserImageConfigure> image_configure(new VpcUserImageConfigure);
  image_configure->widthStride = frame->width;
  image_configure->heightStride = frame->height;

  // check image format is nv12
  if (strcmp(frame->image_format, kImageFormatNv12.c_str()) == kCompareEqual) {
    image_configure->inputFormat = INPUT_YUV420_SEMI_PLANNER_UV;
  } else {  // check image format is nv21
    image_configure->inputFormat = INPUT_YUV420_SEMI_PLANNER_VU;
  }

  image_configure->outputFormat = OUTPUT_YUV420SP_UV;
  image_configure->isCompressData = true;
  image_configure->compressDataConfigure.lumaHeadAddr = (long) (frame->buffer
      + frame->offset_head_y);
  image_configure->compressDataConfigure.chromaHeadAddr = (long) (frame->buffer
      + frame->offset_head_c);
  image_configure->compressDataConfigure.lumaHeadStride = frame->stride_head;
  image_configure->compressDataConfigure.chromaHeadStride = frame->stride_head;
  image_configure->compressDataConfigure.lumaPayloadAddr = (long) (frame->buffer
      + frame->offset_payload_y);
  image_configure->compressDataConfigure.chromaPayloadAddr = (long) (frame
      ->buffer + frame->offset_payload_c);
  image_configure->compressDataConfigure.lumaPayloadStride = frame
      ->stride_payload;
  image_configure->compressDataConfigure.chromaPayloadStride = frame
      ->stride_payload;

  shared_ptr<VpcUserRoiConfigure> roi_configure(new VpcUserRoiConfigure);
  roi_configure->next = nullptr;

  // constructing input roi configuration
  VpcUserRoiInputConfigure *input_configure = &roi_configure->inputConfigure;
  input_configure->cropArea.leftOffset = 0; // 0 means without crop
  // dvpp limits rightOffset is odd
  input_configure->cropArea.rightOffset =
      frame->width % 2 == 0 ? frame->width - 1 : frame->width;
  input_configure->cropArea.upOffset = 0; // 0 means without crop
  // dvpp limits downOffset is odd
  input_configure->cropArea.downOffset =
      frame->height % 2 == 0 ? frame->height - 1 : frame->height;

  int aligned_output_width = ALIGN_UP(frame->width, kVpcWidthAlign);
  int aligned_output_height = ALIGN_UP(frame->height, kVpcHeightAlign);
  int vpc_output_size = aligned_output_width * aligned_output_height
      * DVPP_YUV420SP_SIZE_MOLECULE / DVPP_YUV420SP_SIZE_DENOMINATOR;

  // check vpc output size is valid
  if (vpc_output_size <= 0 || vpc_output_size > kAllowedMaxImageMemory) {
    HIAI_ENGINE_LOG(
        HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
        "The vpc_output_size:%d is invalid! value range: 1~67108864",
        vpc_output_size);
    return;
  }

  // construct vpc out data buffer
  uint8_t *vpc_out_buffer = (uint8_t *) mmap(
      0, vpc_output_size, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | API_MAP_VA32BIT, 0, 0);
  if (vpc_out_buffer == MAP_FAILED) { // check new buffer result
    HIAI_ENGINE_LOG("Failed to malloc memory in dvpp(new vpc).");
    return;
  }

  // constructing output roi configuration
  VpcUserRoiOutputConfigure *output_configure = &roi_configure->outputConfigure;
  output_configure->addr = vpc_out_buffer;
  output_configure->bufferSize = vpc_output_size;
  output_configure->widthStride = frame->width;
  output_configure->heightStride = frame->height;
  output_configure->outputArea.leftOffset = 0; // 0 means without crop
  // dvpp limits rightOffset is odd
  output_configure->outputArea.rightOffset =
      frame->width % 2 == 0 ? frame->width - 1 : frame->width;
  output_configure->outputArea.upOffset = 0; // 0 means without crop
  // dvpp limits downOffset is odd
  output_configure->outputArea.downOffset =
      frame->height % 2 == 0 ? frame->height - 1 : frame->height;

  image_configure->roiConfigure = roi_configure.get();

  dvppapi_ctl_msg dvpp_api_ctl_msg;
  dvpp_api_ctl_msg.in = static_cast<void *>(image_configure.get());
  dvpp_api_ctl_msg.in_size = sizeof(VpcUserImageConfigure);

  // call vpc and check the result
  if (DvppCtl(dvpp_api, DVPP_CTL_VPC_PROC, &dvpp_api_ctl_msg)
      != kHandleSuccessful) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to call dvppctl VPC!");
    // free vpc_out_buffer memory
    munmap(vpc_out_buffer, (unsigned) (ALIGN_UP(vpc_output_size, MAP_2M)));
    DestroyDvppApi(dvpp_api);
    return;
  }

  DestroyDvppApi(dvpp_api);

  uint8_t* output_image_buffer = new (nothrow) uint8_t[vpc_output_size];
  if (output_image_buffer == nullptr) { // check new result
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to new data when handle vpc output!");
    // free vpc_out_buffer memory
    munmap(vpc_out_buffer, (unsigned) (ALIGN_UP(vpc_output_size, MAP_2M)));
    return;
  }

  int memcpy_result = memcpy_s(output_image_buffer, vpc_output_size,
                               vpc_out_buffer, vpc_output_size);
  // free vpc_out_buffer memory
  munmap(vpc_out_buffer, (unsigned) (ALIGN_UP(vpc_output_size, MAP_2M)));

  if (memcpy_result != EOK) { // check memcpy_s result
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to copy vpc output image buffer, memcpy_s result:%d",
                    memcpy_result);
    delete[] output_image_buffer;
    return;
  }

  // send key frame data to next engine
  SendKeyFrameData(output_image_buffer, vpc_output_size, hiai_data, frame);
  return;
}

int VideoDecode::GetVideoIndex(AVFormatContext* av_format_context) {
  if (av_format_context == nullptr) {  // verify input pointer
    return kInvalidVideoIndex;
  }

  // get video index in streams
  for (int i = 0; i < av_format_context->nb_streams; i++) {
    if (av_format_context->streams[i]->codecpar->codec_type
        == AVMEDIA_TYPE_VIDEO) {  // check is media type is video
      return i;
    }
  }

  return kInvalidVideoIndex;
}

bool VideoDecode::CheckVideoType(int video_index,
                                 AVFormatContext* av_format_context,
                                 VideoType &video_type) {
  AVStream* in_stream = av_format_context->streams[video_index];
  HIAI_ENGINE_LOG("Display video stream resolution, width:%d, height:%d",
                  in_stream->codecpar->width, in_stream->codecpar->height);

  if (in_stream->codecpar->codec_id == AV_CODEC_ID_H264) { // video type: h264
    video_type = kH264;
    HIAI_ENGINE_LOG("Video type:H264");
    return true;
  } else if (in_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) { // h265
    video_type = kH265;
    HIAI_ENGINE_LOG("Video type:H265");
    return true;
  } else { // the video type is invalid
    video_type = kInvalidTpye;
    HIAI_ENGINE_LOG(
        "The video type is invalid, should be h264 or h265"
        "AVCodecID:%d(detail type please to view enum AVCodecID in ffmpeg)",
        in_stream->codecpar->codec_id);
    return false;
  }
}

void VideoDecode::InitVideoStreamFilter(
    VideoType video_type, const AVBitStreamFilter* &video_filter) {
  if (video_type == kH264) { // check video type is h264
    video_filter = av_bsf_get_by_name("h264_mp4toannexb");
  } else { // the video type is h265
    video_filter = av_bsf_get_by_name("hevc_mp4toannexb");
  }
}

int VideoDecode::GetIntChannelId(const string channel_id) {
  if (channel_id == kStrChannelId1) { // check channel is channel1
    return kIntChannelId1;
  } else { // the channel is channel2
    return kIntChannelId2;
  }
}

const string &VideoDecode::GetChannelValue(const string &channel_id) {
  if (channel_id == kStrChannelId1) { // check channel is channel1
    return channel1_;
  } else { // the channel is channel2
    return channel2_;
  }
}

void VideoDecode::SetDictForRtsp(const string& channel_value,
                                 AVDictionary* &avdic) {
  if (IsValidRtsp(channel_value)) { // check channel value is valid rtsp address
    HIAI_ENGINE_LOG("Set parameters for %s", channel_value.c_str());
    avformat_network_init();

    av_dict_set(&avdic, kRtspTransport.c_str(), kUdp.c_str(), kNoFlag);
    av_dict_set(&avdic, kBufferSize.c_str(), kMaxBufferSize.c_str(), kNoFlag);
    av_dict_set(&avdic, kMaxDelayStr.c_str(), kMaxDelayValue.c_str(), kNoFlag);
    av_dict_set(&avdic, kTimeoutStr.c_str(), kTimeoutValue.c_str(), kNoFlag);
    av_dict_set(&avdic, kReorderQueueSize.c_str(),
                kReorderQueueSizeValue.c_str(), kNoFlag);
    av_dict_set(&avdic, kPktSize.c_str(), kPktSizeValue.c_str(), kNoFlag);
  }
}

bool VideoDecode::OpenVideoFromInputChannel(
    const string &channel_value, AVFormatContext* &av_format_context) {
  AVDictionary* avdic = nullptr;
  SetDictForRtsp(channel_value, avdic);

  int ret_open_input_video = avformat_open_input(&av_format_context,
                                                 channel_value.c_str(), nullptr,
                                                 &avdic);

  if (ret_open_input_video < kHandleSuccessful) { // check open video result
    char buf_error[kErrorBufferSize];
    av_strerror(ret_open_input_video, buf_error, kErrorBufferSize);

    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT, "Could not open video:%s, "
                    "result of avformat_open_input:%d, error info:%s",
                    channel_value.c_str(), ret_open_input_video, buf_error);

    if (avdic != nullptr) { // free AVDictionary
      av_dict_free(&avdic);
    }

    return false;
  }

  if (avdic != nullptr) { // free AVDictionary
    av_dict_free(&avdic);
  }

  return true;
}

bool VideoDecode::InitVideoParams(int videoindex, VideoType &video_type,
                                  AVFormatContext* av_format_context,
                                  AVBSFContext* &bsf_ctx) {
  // check video type, only support h264 and h265
  if (!CheckVideoType(videoindex, av_format_context, video_type)) {
    avformat_close_input(&av_format_context);

    return false;
  }

  const AVBitStreamFilter* video_filter;
  InitVideoStreamFilter(video_type, video_filter);
  if (video_filter == nullptr) { // check video fileter is nullptr
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Unkonw bitstream filter, video_filter is nullptr!");
    return false;
  }

  // checke alloc bsf context result
  if (av_bsf_alloc(video_filter, &bsf_ctx) < kHandleSuccessful) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to call av_bsf_alloc!");
    return false;
  }

  // check copy parameters result
  if (avcodec_parameters_copy(bsf_ctx->par_in,
                              av_format_context->streams[videoindex]->codecpar)
      < kHandleSuccessful) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to call avcodec_parameters_copy!");
    return false;
  }

  bsf_ctx->time_base_in = av_format_context->streams[videoindex]->time_base;

  // check initialize bsf contextreult
  if (av_bsf_init(bsf_ctx) < kHandleSuccessful) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to call av_bsf_init!");
    return false;
  }

  return true;
}

void VideoDecode::UnpackVideo2Image(const string &channel_id) {
  char thread_name_log[kThreadNameLength];
  string thread_name = kThreadNameHead + channel_id;
  prctl(PR_SET_NAME, (unsigned long) thread_name.c_str());
  prctl(PR_GET_NAME, (unsigned long) thread_name_log);
  HIAI_ENGINE_LOG("Unpack video to image from:%s, thread name:%s",
                  channel_id.c_str(), thread_name_log);

  string channel_value = GetChannelValue(channel_id);
  AVFormatContext* av_format_context = avformat_alloc_context();

  // check open video result
  if (!OpenVideoFromInputChannel(channel_value, av_format_context)) {
    return;
  }

  int videoindex = GetVideoIndex(av_format_context);
  if (videoindex == kInvalidVideoIndex) { // check video index is valid
    HIAI_ENGINE_LOG(
        HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
        "Video index is -1, current media has no video info, channel id:%s",
        channel_id.c_str());
    return;
  }

  AVBSFContext* bsf_ctx;
  VideoType video_type = kInvalidTpye;

  // check initialize video parameters result
  if (!InitVideoParams(videoindex, video_type, av_format_context, bsf_ctx)) {
    return;
  }

  IDVPPAPI* dvpp_api = nullptr;
  CreateVdecApi(dvpp_api, 0);
  if (dvpp_api == nullptr) { // check create dvpp api result
    DestroyVdecApi(dvpp_api, 0);
    return;
  }

  vdec_in_msg vdec_msg;
  vdec_msg.call_back = CallVpcGetYuvImage;

  YuvImageFrameInfo yuv_image_frame_info;
  yuv_image_frame_info.channel_name = channel_value;
  yuv_image_frame_info.channel_id = channel_id;

  vdec_msg.channelId = GetIntChannelId(channel_id);
  vdec_msg.hiai_data = &yuv_image_frame_info;

  int strcpy_result = 0;
  if (video_type == kH264) { // check video type is h264
    strcpy_result = strcpy_s(vdec_msg.video_format, kVideoFormatLength,
                             kVideoTypeH264.c_str());
  } else { // the video type is h264
    strcpy_result = strcpy_s(vdec_msg.video_format, kVideoFormatLength,
                             kVideoTypeH265.c_str());
  }

  if (strcpy_result != EOK) { // check strcpy result
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Fail to call strcpy_s, result:%d, channel id:%s",
                    strcpy_result, channel_id.c_str());
    return;
  }

  dvppapi_ctl_msg dvppApiCtlMsg;
  dvppApiCtlMsg.in = (void*) (&vdec_msg);
  dvppApiCtlMsg.in_size = sizeof(vdec_in_msg);

  AVPacket av_packet;
  // loop to get every frame from video stream
  while (av_read_frame(av_format_context, &av_packet) == kHandleSuccessful) {
    if (av_packet.stream_index == videoindex) { // check current stream is video
      // send video packet to ffmpeg
      if (av_bsf_send_packet(bsf_ctx, &av_packet) != kHandleSuccessful) {
        HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                        "Fail to call av_bsf_send_packet, channel id:%s",
                        channel_id.c_str());
      }

      // receive single frame from ffmpeg
      while (av_bsf_receive_packet(bsf_ctx, &av_packet) == kHandleSuccessful) {
        vdec_msg.in_buffer = (char*) av_packet.data;
        vdec_msg.in_buffer_size = av_packet.size;

        // call vdec and check result
        if (VdecCtl(dvpp_api, DVPP_CTL_VDEC_PROC, &dvppApiCtlMsg, 0)
            != kHandleSuccessful) {
          HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                          "Fail to call dvppctl process, channel id:%s",
                          channel_id.c_str());

          av_packet_unref(&av_packet);
          av_bsf_free(&bsf_ctx);
          avformat_close_input(&av_format_context);
          DestroyVdecApi(dvpp_api, 0);
          return;
        }

        av_packet_unref(&av_packet);

        // send image data to next engine
        SendImageDate(channel_id);
      }
    }
  }

  av_bsf_free(&bsf_ctx);  // free AVBSFContext pointer
  avformat_close_input(&av_format_context);  // close input video
  DestroyVdecApi(dvpp_api, 0);

  // send last yuv image data after call vdec
  SendImageDate(channel_id);

  HIAI_ENGINE_LOG("Ffmpeg read frame finished, channel id:%s",
                  channel_id.c_str());
}

bool VideoDecode::VerifyVideoWithUnpack(const string &channel_value) {
  HIAI_ENGINE_LOG("Start to verify unpack video file:%s",
                  channel_value.c_str());

  AVFormatContext* av_format_context = avformat_alloc_context();
  AVDictionary* avdic = nullptr;
  SetDictForRtsp(channel_value, avdic);

  int ret_open_input_video = avformat_open_input(&av_format_context,
                                                 channel_value.c_str(), nullptr,
                                                 &avdic);
  if (ret_open_input_video < kHandleSuccessful) { // check open video result
    char buf_error[kErrorBufferSize];
    av_strerror(ret_open_input_video, buf_error, kErrorBufferSize);

    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT, "Could not open video:%s, "
                    "result of avformat_open_input:%d, error info:%s",
                    channel_value.c_str(), ret_open_input_video, buf_error);

    if (avdic != nullptr) { // free AVDictionary
      av_dict_free(&avdic);
    }

    return false;
  }

  if (avdic != nullptr) { // free AVDictionary
    av_dict_free(&avdic);
  }

  int video_index = GetVideoIndex(av_format_context);
  if (video_index == kInvalidVideoIndex) { // check video index is valid
    HIAI_ENGINE_LOG(
        HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
        "Video index is -1, current media stream has no video info:%s",
        channel_value.c_str());

    avformat_close_input(&av_format_context);
    return false;
  }

  VideoType video_type = kInvalidTpye;
  bool is_valid = CheckVideoType(video_index, av_format_context, video_type);
  avformat_close_input(&av_format_context);

  return is_valid;
}

bool VideoDecode::VerifyVideoType() {
  // check channel1 and channel2 is not empty
  if (!IsEmpty(channel1_, kStrChannelId1)
      && !IsEmpty(channel2_, kStrChannelId2)) {
    return (VerifyVideoWithUnpack(channel1_) && VerifyVideoWithUnpack(channel2_));
  } else if (!IsEmpty(channel1_, kStrChannelId1)) { // channel1 is not empty
    return VerifyVideoWithUnpack(channel1_);
  } else {  // channel2 is not empty
    return VerifyVideoWithUnpack(channel2_);
  }
}

void VideoDecode::MultithreadHandleVideo() {
  // create two thread unpacke channel1 and channel2 video in same time
  if (!IsEmpty(channel1_, kStrChannelId1)
      && !IsEmpty(channel2_, kStrChannelId2)) {
    thread thread_channel_1(&VideoDecode::UnpackVideo2Image, this,
                            kStrChannelId1);
    thread thread_channel_2(&VideoDecode::UnpackVideo2Image, this,
                            kStrChannelId2);

    thread_channel_1.join();
    thread_channel_2.join();
  } else if (!IsEmpty(channel1_, kStrChannelId1)) { // unpacke channel1 video
    UnpackVideo2Image(kStrChannelId1);
  } else {  // unpacke channel2 video
    UnpackVideo2Image(kStrChannelId2);
  }
}

HIAI_StatusT VideoDecode::Init(
    const hiai::AIConfig &config,
    const vector<hiai::AIModelDescription> &model_desc) {
  HIAI_ENGINE_LOG("Start process!");

  // get channel values from configs item
  for (int index = 0; index < config.items_size(); ++index) {
    const ::hiai::AIConfigItem &item = config.items(index);

    // get channel1 value
    if (item.name() == kStrChannelId1) {
      channel1_ = item.value();
      continue;
    }

    // get channel2 value
    if (item.name() == kStrChannelId2) {
      channel2_ = item.value();
      continue;
    }
  }

  // verify channel values are valid
  if (!VerifyChannelValues()) {
    return HIAI_ERROR;
  }

  HIAI_ENGINE_LOG("End process!");
  return HIAI_OK;
}

bool VideoDecode::IsEmpty(const string &input_str, const string &channel_id) {
  regex regex_space(kRegexSpace.c_str());

  // check input string is empty or spaces
  if (regex_match(input_str, regex_space)) {
    HIAI_ENGINE_LOG("The channel string is empty or all spaces, channel id:%s",
                    channel_id.c_str());
    return true;
  }

  return false;
}

bool VideoDecode::VerifyVideoSourceName(const string &input_str) {
  // verify input string is valid mp4 file name or rtsp address
  if (!IsValidRtsp(input_str) && !IsValidMp4File(input_str)) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Invalid mp4 file name or RTSP name:%s", input_str.c_str());
    return false;
  }

  return true;
}

bool VideoDecode::IsValidRtsp(const string &input_str) {
  regex regex_rtsp_address(kRegexRtsp.c_str());

  // verify input string is valid rtsp address
  if (regex_match(input_str, regex_rtsp_address)) {
    return true;
  }

  return false;
}

bool VideoDecode::IsValidMp4File(const string &input_str) {
  regex regex_mp4_file_name(kRegexMp4File.c_str());

  // verify input string is valid mp4 file name
  if (regex_match(input_str, regex_mp4_file_name)) {
    return true;
  }

  return false;
}

bool VideoDecode::VerifyChannelValues() {
  // check channel1 and channel2 are empty
  if (IsEmpty(channel1_, kStrChannelId1)
      && IsEmpty(channel2_, kStrChannelId2)) {
    HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                    "Both channel1 and channel2 are empty!");
    return false;
  }

  // verify channel1 value when channel1 is not empty
  if (!IsEmpty(channel1_, kStrChannelId1)) {
    // deletes the space at the head of the string
    channel1_.erase(0, channel1_.find_first_not_of(kNeedRemoveStr.c_str()));

    // deletes spaces at the end of the string
    channel1_.erase(channel1_.find_last_not_of(kNeedRemoveStr.c_str()) + 1);

    HIAI_ENGINE_LOG("Display channel1:%s", channel1_.c_str());

    if (!VerifyVideoSourceName(channel1_)) { // verify channel1
      return false;
    }
  }

  // verify channel2 value when channel1 is not empty
  if (!IsEmpty(channel2_, kStrChannelId2)) {
    // deletes the space at the head of the string
    channel2_.erase(0, channel2_.find_first_not_of(kNeedRemoveStr.c_str()));

    // deletes spaces at the end of the string
    channel2_.erase(channel2_.find_last_not_of(kNeedRemoveStr.c_str()) + 1);

    HIAI_ENGINE_LOG("Display channel2:%s", channel2_.c_str());

    if (!VerifyVideoSourceName(channel2_)) { // verify channel2
      return false;
    }
  }

  return true;
}

void VideoDecode::SendImageDataByChannel(
    ThreadSafeQueue<shared_ptr<VideoImageParaT>> &current_queue) {
  HIAI_StatusT hiai_ret = HIAI_OK;

  // send image data unitl queue is empty
  while (!current_queue.empty()) {
    shared_ptr<VideoImageParaT> video_iamge_data = current_queue.Pop();
    if (video_iamge_data == nullptr) { // the queue is empty and return
      return;
    }

    // send image data
    do {
      hiai_ret = SendData(0, kVideoImageParaType,
                          static_pointer_cast<void>(video_iamge_data));
      if (hiai_ret == HIAI_QUEUE_FULL) { // check queue is full
        HIAI_ENGINE_LOG("The queue is full when send image data, sleep 10ms");
        usleep(kWait10Milliseconds); // sleep 10 ms
      }
    } while (hiai_ret == HIAI_QUEUE_FULL); // loop while queue is full

    if (hiai_ret != HIAI_OK) { // check send data is failed
      HIAI_ENGINE_LOG(HIAI_ENGINE_RUN_ARGS_NOT_RIGHT,
                      "Send data failed! error code: %d", hiai_ret);
    }
  }
}

void VideoDecode::SendImageDate(const string &channel_id) {
  if (channel_id == kStrChannelId1) { // check channel id is channle1
    SendImageDataByChannel(channel1_queue);
  } else { // check channel id is channle2
    SendImageDataByChannel(channel2_queue);
  }
}

HIAI_IMPL_ENGINE_PROCESS("video_decode", VideoDecode, INPUT_SIZE) {
  av_log_set_level(AV_LOG_INFO);  // set ffmpeg log level

  // verify video type
  if (!VerifyVideoType()) {
    SendFinishedData();  // send the flag data when finished
    return HIAI_ERROR;
  }

  MultithreadHandleVideo();  // handle video from file or RTSP with multi-thread

  SendFinishedData();  // send the flag data when finished

  return HIAI_OK;
}
