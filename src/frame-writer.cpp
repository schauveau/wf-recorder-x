// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp
//
// Audio encoding - thanks to wlstream, a lot of the code/ideas are taken from there

#include <iostream>
#include "frame-writer.hpp"
#include <vector>
#include <queue>
#include <cstring>
#include "averr.h"
#include <iomanip>

#define AUDIO_RATE 44100

static const AVRational US_RATIONAL{1,1000000} ; // = 1us as a AVRational

inline std::ostream & operator<<(std::ostream &out, AVRational r) {
  out << r.num << '/' << r.den ;
  return out;
}

static inline bool is_fmt_supported(AVPixelFormat fmt, const AVPixelFormat *supported)
{
  for (int i = 0; supported[i] != AV_PIX_FMT_NONE; i++) {
    if (supported[i] == fmt)
      return true;
  }
  return false;
}

class FFmpegInitialize
{
public :
  FFmpegInitialize()
  {
    // Loads the whole database of available codecs and formats.
    av_register_all();
  }
};

static FFmpegInitialize ffmpegInitialize;


#define forall_codecs(var) for ( void * var##_it = NULL ; (codec=av_codec_iterate(&var##_it)) ; )
#define forall_output_formats(var) for ( void * var##_it = NULL ; (var=av_muxer_iterate(&var##_it)) ; )

#define hex_format(val) "0x" << std::hex << std::setw(sizeof(val)*2) << std::setfill('0') << val << std::setfill(' ')

void
FrameWriter::dump_available_encoders(std::ostream &out)
{
  const AVCodec *codec = NULL ;
  out << "Available encoders:" << std::endl ; 
  //for ( void * it = NULL ; (codec=av_codec_iterate(&it)) ; ) {
  forall_codecs(codec) {
    if ( codec->type != AVMEDIA_TYPE_VIDEO )
      continue ;
    if (!av_codec_is_encoder(codec))
      continue ;
    out << std::left << std::setw(18) << codec->name  ;    
    //    out <<  " caps:    " << hex_format(codec->capabilities) ;
    out <<  " (" << codec->long_name << ")" ;


    // TODO: Dump HW information
    //
    // Remark: as of ffmpeg 4.1, avcodec_get_hw_config() does not work.
    // It only appers to be set for a small subset of the encoders?
    //
    // while( const AVCodecHWConfig * hwc = avcodec_get_hw_config(codec,index++) ) {
    //  out << " hw=" << hwc->device_type ;
    // }
    //
    out << "\n" ;
  }

  out << "\nOutput formats:\n" ;

  void *it = NULL;
  while (true) {
    const AVOutputFormat * fmt = av_muxer_iterate(&it) ;    
    if (!fmt)
      break ;
    if (fmt->video_codec == AV_CODEC_ID_NONE)
      continue ;
    if ( fmt->flags & AVFMT_NEEDNUMBER ) {
      //continue ;
    }
    out << std::left << std::setw(15) << fmt->name ;

    out << " " << hex_format(fmt->flags) ;

    if ( fmt->extensions ) {
      out <<" (" << fmt->extensions << ") ";
    }
    
#define DO_FLAG(f) if ( fmt->flags & AVFMT_##f ) { out << " " << #f ; } 
    DO_FLAG(NOFILE);
    DO_FLAG(NEEDNUMBER);
    //DO_FLAG(SHOW_IDS);
    //DO_FLAG(GLOBALHEADER);
    //DO_FLAG(NOTIMESTAMPS);
    //DO_FLAG(GENERIC_INDEX);
    //DO_FLAG(TS_DISCONT);
    DO_FLAG(VARIABLE_FPS);
    //DO_FLAG(NODIMENSIONS);
    //DO_FLAG(NOSTREAMS);
    //DO_FLAG(NOBINSEARCH);
    //DO_FLAG(NOGENSEARCH);
    //DO_FLAG(NO_BYTE_SEEK);
    //DO_FLAG(ALLOW_FLUSH);
    //DO_FLAG(TS_NONSTRICT);
    //DO_FLAG(TS_NEGATIVE);
    //DO_FLAG(SEEK_TO_PTS);
    out << "\n" ;
    //out << fmt->name << "\n";
  }
  out << ".\n";
  
}

void FrameWriter::init_hw_accel()
{
  if ( params.hw_method == "" )
    return ; 

  AVHWDeviceType hwtype = av_hwdevice_find_type_by_name( params.hw_method.c_str() );
  if ( hwtype == AV_HWDEVICE_TYPE_NONE) {
    std::cerr << "Unknowns or unavailable HW method '" << params.hw_method << "'\n";
    std::exit(-1);
  }

  int ret = av_hwdevice_ctx_create(&this->hw_device_context,
                                   hwtype,
                                   params.hw_device.c_str(),
                                   NULL,
                                   0);
  if (ret != 0)
    {
      std::cerr << "Failed to create HW device '" << params.hw_device
                << "': " << averr(ret) << std::endl;
      std::exit(-1);
    }

}

void FrameWriter::load_codec_options(AVDictionary **dict)
{
  // TODO: move defaults to main() 
  static const std::map<std::string, std::string> default_x264_options =
    {
#if 0
     {"tune", "zerolatency"},
     {"preset", "ultrafast"},
     {"crf", "20"},
#endif
    };
  
  if (params.codec.find("libx264") != std::string::npos ||
      params.codec.find("libx265") != std::string::npos)
    {
      for (const auto& param : default_x264_options)
        {
          if (!params.codec_options.count(param.first))
            params.codec_options[param.first] = param.second;
        }
    }

  for (auto& opt : params.codec_options)
    {
      std::cout << "Setting codec option: " << opt.first << "=" << opt.second << std::endl;
      av_dict_set(dict, opt.first.c_str(), opt.second.c_str(), 0);
    }
}

AVPixelFormat FrameWriter::get_input_format()
{
  // TODO: use RGBA instead of  RGB0 because of the 'overlay' filter.
  //       ==> Should set it back to RGB0 before the encoder.
  //       ==> Manually? can a filter do that?
  return params.format == INPUT_FORMAT_BGR0 ?
    AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
}

static void sort_filter_graph(AVFilterGraph *graph)
{ 
  // The filters are unsorted by default which makes the graph
  // less readable. Let's try to solve that.
  // However, we have to be carefull because graphs may have
  // cycles (probably not but let's play safe).
  int nf = graph->nb_filters;
  AVFilterContext **all = graph->filters ;
  int n1; 
  for (int n0=0; n0<nf ; n0=n1) {
    // The range [0..n0) is already sorted
    n1=n0;
    for (int i=n0;i<nf;i++){
      AVFilterContext *f = all[i];      
      bool match = (f->nb_inputs==0);
      for(unsigned int j=0; j < f->nb_inputs ; j++) {
        AVFilterLink *link = f->inputs[j];                
        for (int k=0;k<n0;k++) {
          match = ( all[k] == link->src ) ;
          if (match) break ;
        }
      }
      if (match) {
        std::swap( all[i], all[n1++] ) ;
        break;
      }
    }   
    if (n0==n1) {
      break ; // No progress! Give upp
    }
  }
}

void FrameWriter::init_video_filters(AVCodec *codec)
{
  int err;

  AVFilterGraph *filter_graph = avfilter_graph_alloc();
 
  const AVFilter * source = avfilter_get_by_name("buffer");
  const AVFilter * sink   = avfilter_get_by_name("buffersink");
  
  if (!source || !sink) {
    std::cerr << "filtering source or sink element not found\n";
    exit(-1);
  }

  // Build the configuration of the 'buffer' filter.
  // See: ffmpeg -h filter=buffer
  // See: https://ffmpeg.org/ffmpeg-filters.html#buffer

  const int sz=500 ; // TODO: use a std::stringstream?
  char source_args[sz];
  err = snprintf(source_args, sz, 
                 "video_size=%dx%d"
                 ":pix_fmt=%d"
                 ":time_base=%d/%d"
                 ":pixel_aspect=%d/%d"
                 ":sws_param=flags=fast_bilinear"  
                 ,
                 params.width, params.height,      // video size
                 int(this->get_input_format()),    // pix_fmt             
                 US_RATIONAL.num, US_RATIONAL.den, // time_base. We use micro-seconds
                 1,1                               // pixel_aspect
                 /* ... */                         // sws_param
                 );
  if (err >= sz) {
    std::cerr << "I am paranoid" << std::endl;;
    exit(-1);
  }
 
  AVFilterContext *source_ctx = NULL;
  err = avfilter_graph_create_filter(&source_ctx, source, "Source",
                                     source_args, NULL, filter_graph);
  if (err < 0) {
    std::cerr << "Cannot create video filter in: " << averr(err) << std::endl;;
    exit(-1);
  }
  

  AVFilterContext * sink_ctx = NULL ;
  err = avfilter_graph_create_filter(&sink_ctx, sink, "Sink",
                                     NULL, NULL, filter_graph);
  if (err < 0) {
    std::cerr << "Cannot create video filter out: " << averr(err) << std::endl;;
    exit(-1);
  }

  // We also need to tell the sink which pixel formats are supported.
  // by the video encoder. codevIndicate to our sink  pixel formats
  // are accepted by our codec. 

  if (true) {
    std::cerr << "Available encoding pixel formats: ";
    for ( auto *p = codec->pix_fmts ; *p != AV_PIX_FMT_NONE ; p++) {
      if (p!=codec->pix_fmts)
        std::cerr <<",";
      std::cerr << av_get_pix_fmt_name(*p) ;
    }
    std::cerr << "\n";
  }

  const AVPixelFormat *supported_pix_fmts = codec->pix_fmts;
  static const AVPixelFormat only_yuv420p[] =
    {
     AV_PIX_FMT_YUV420P,
     AV_PIX_FMT_NONE
    } ;

  // Force the pixel format to yuv420p on user request (-to-yuv)
  // but only if the codec supports that value.
  // TODO: An option to select any pixel format would be nice.
  if ( params.to_yuv ) { 
    if ( is_fmt_supported(AV_PIX_FMT_YUV420P, supported_pix_fmts) ) {
      supported_pix_fmts = only_yuv420p ;
    } else {
      std::cerr << "Ignoring request to force yuv420p.\n";
    }
  }
  
  err = av_opt_set_int_list(sink_ctx, "pix_fmts", supported_pix_fmts,
                            AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

  if (err < 0) {
    std::cerr << "Failed to set pix_fmts: " << averr(err) << std::endl;;
    exit(-1);
  }

  // Create the connections to the filter graph
  //
  // The in/out swap is not a mistake:
  //
  //   ----------       -----------------------------      --------
  //   | Source | ----> | in -> filter_graph -> out | ---> | Sink | 
  //   ----------       -----------------------------      --------
  //
  // The 'in' of filter_graph is the output of the Source buffer
  // The 'out' of filter_graph is the input of the Sink buffer
  //
  
  AVFilterInOut *outputs = avfilter_inout_alloc();
  outputs->name       = av_strdup("in");
  outputs->filter_ctx = source_ctx; 
  outputs->pad_idx    = 0;
  outputs->next       = NULL;
  
  AVFilterInOut *inputs  = avfilter_inout_alloc();
  inputs->name       = av_strdup("out");
  inputs->filter_ctx = sink_ctx;
  inputs->pad_idx    = 0;
  inputs->next       = NULL;
  
  if (!outputs->name || !inputs->name) {
    std::cerr << "Failed to parse allocate inout filter links" << std::endl ;
    exit(-1);
  }

  // This is the configuration of the automatic 'scale' filtered that will
  // often be inserted to get from RGB to YUV. The problem here is that
  // that the default behavior is to set 
  // TODO: Make that configurable (as in ffmpeg -scale_sws_opts)
  //
  
#if 1
  //filter_graph->scale_sws_opts = av_strdup("in_range=pc");
  filter_graph->scale_sws_opts = av_strdup("in_range=pc:out_range=pc");
  //filter_graph->scale_sws_opts = av_strdup("in_range=tv:out_range=tv");
#endif
  
  std::string filter_text = params.video_filter ;
  if ( filter_text.empty() ) {
    filter_text = "null" ;     // "null" is the dummy video filter
  }
  std::cerr << "Using video filter: " << filter_text << std::endl;;    

  err = avfilter_graph_parse_ptr(filter_graph,
                                 filter_text.c_str(),
                                 &inputs,
                                 &outputs,
                                 NULL);
  if (err < 0) {
    std::cerr << "Failed to parse graph filter: " << averr(err) << std::endl;;    
    exit(-1) ;
  }  

  // Filters that create HW frames ('hwupload', 'hwmap', ...) need
  // AVBufferRef in their hw_device_ctx. Unfortunately, there is no
  // simple API to do that for filters created by avfilter_graph_parse_ptr().
  // The code below is inspired from ffmpeg_filter.c
  if (this->hw_device_context) {
    for (unsigned i=0; i<filter_graph->nb_filters; i++) {
      filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(this->hw_device_context);
      if (!filter_graph->filters[i]->hw_device_ctx) {
        std::cerr << "Failed to create ref to HW context\n";
        exit(-1);
      }
    }
  }

  err = avfilter_graph_config(filter_graph, NULL);
  if (err<0) {
    std::cerr << "Failed to configure graph filter: " << averr(err) << std::endl;;    
    exit(-1) ;
  }

  // TODO: make the dump optional
  if (1) {

    sort_filter_graph(filter_graph);
    
    std::cout << std::string(80,'#') << std::endl ;
    std::cout << avfilter_graph_dump(filter_graph,0) << "\n";
    std::cout << std::string(80,'#') << std::endl ;     
  }
  
  // The (input of the) sink is the output of the whole filter.  
  AVFilterLink * filter_output = sink_ctx->inputs[0] ;
  
  this->vfilter.width  = filter_output->w ;
  this->vfilter.height = filter_output->h ;
  this->vfilter.sar    = filter_output->sample_aspect_ratio ; 
  this->vfilter.pix_fmt = (AVPixelFormat) filter_output->format ;
  this->vfilter.time_base = filter_output->time_base;
  this->vfilter.frame_rate = filter_output->frame_rate; // can be 1/0 if unknown

  this->hw_frame_context = av_buffersink_get_hw_frames_ctx(sink_ctx); 
    
  std::cerr << "Encoding input format:"
            << " w=" << this->vfilter.width
            << " h=" << this->vfilter.height
            << " pixfmt=" << av_get_pix_fmt_name(this->vfilter.pix_fmt)
            << " sample_aspect_ratio=" << this->vfilter.sar
            << " time_base=" << this->vfilter.time_base
            << " frame_rate=" << this->vfilter.frame_rate
            << "\n";
      
  // TODO: free the graph in destructor
  this->videoFilterGraph = filter_graph;
  this->videoFilterSourceCtx = source_ctx;
  this->videoFilterSinkCtx = sink_ctx;
  
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
}

void FrameWriter::init_video_stream()
{
  AVDictionary *options = NULL;
  load_codec_options(&options);
  init_hw_accel();
    
  std::cerr << "Using encoder '" << params.codec << "'" << std::endl;
  AVCodec* codec = avcodec_find_encoder_by_name(params.codec.c_str());
  if (!codec)
    {
      std::cerr << "Failed to find the specified encoder: " << params.codec << std::endl;
      std::exit(-1);
    }

  init_video_filters(codec);
    
  videoStream = avformat_new_stream(fmtCtx, codec);
  if (!videoStream)
    {
      std::cerr << "Failed to open stream" << std::endl;
      std::exit(-1);
    }

  videoCodecCtx = videoStream->codec;   
    
  videoCodecCtx->width      = vfilter.width;
  videoCodecCtx->height     = vfilter.height;
  videoCodecCtx->time_base  = vfilter.time_base; // may be changed by avcodec_open2
  videoCodecCtx->sample_aspect_ratio = vfilter.sar ;

  // This is just a hint. Some encoders will change the value to AVCOL_RANGE_MPEG
  // (e.g. vp9_vaapi)
  videoCodecCtx->color_range = AVCOL_RANGE_JPEG ;
  
  // Note: since out input if RGB, FFMpeg will usually select
  // YUV444p over the more common but less accurate YUV420p.
  // Can be changed by a 'format=yuv420p' filter or by
  // the --to-yuv option.
  // Remark: Setting 
  videoCodecCtx->pix_fmt    = vfilter.pix_fmt ;
    
  std::cerr << "Selected encoding pixel format = "
            << av_get_pix_fmt_name(videoCodecCtx->pix_fmt) << std::endl;

  // Does that matter?   
  videoCodecCtx->framerate = vfilter.frame_rate ;

  if ( this->hw_frame_context ) {
    videoCodecCtx->hw_frames_ctx = av_buffer_ref(this->hw_frame_context);
  }   

  if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
    videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int err;
  err = avcodec_open2(videoCodecCtx, codec, &options);
  if (err < 0) {
    std::cerr << "avcodec_open2 failed: " << averr(err) << std::endl;
    std::exit(-1);
  }
  av_dict_free(&options);

#if 1
  // 
  // Those are taken from ffmpeg.c but I am not sure that they are really needed
  // because their videoCodecCtx is not equal to their videoStream->codec
  //
  //
  err = avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx);
  if (err < 0) {
    std::cerr << "avcodec_parameters_from_context failed: " << averr(err) << std::endl;
    std::exit(-1);
  }

  
  if (videoCodecCtx->nb_coded_side_data) {
    int i;
    for (i = 0; i < videoCodecCtx->nb_coded_side_data; i++) {
      const AVPacketSideData *sd_src = &videoCodecCtx->coded_side_data[i];
      uint8_t *dst_data = av_stream_new_side_data(videoStream, sd_src->type, sd_src->size);
      if (!dst_data) {
        std::cerr << "bad side data" << std::endl;
        std::exit(-1);
      }
      memcpy(dst_data, sd_src->data, sd_src->size);
    }
  }
#endif
  
  // Use the same time_base for the stream than for the codec.
  // This is not strictly necessary but that makes sense.
  // Reminder:
  //   The value written now is just a hint for avformat_write_header().
  //   It can be modified so we cannot avoid calling av_packet_rescale_ts()
  //   on all packets.
  videoStream->time_base = US_RATIONAL;
}

static uint64_t get_codec_channel_layout(AVCodec *codec)
{
  int i = 0;
  if (!codec->channel_layouts)
    return AV_CH_LAYOUT_STEREO;
  while (1) {
    if (!codec->channel_layouts[i])
      break;
    if (codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
      return codec->channel_layouts[i];
    i++;
  }
  return codec->channel_layouts[0];
}

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec)
{
  int i = 0;
  if (!codec->sample_fmts)
    return AV_SAMPLE_FMT_S16;
  while (1) {
    if (codec->sample_fmts[i] == -1)
      break;
    if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
      return codec->sample_fmts[i];
    i++;
  }
  return codec->sample_fmts[0];
}

void FrameWriter::init_audio_stream()
{
  AVCodec* codec = avcodec_find_encoder_by_name("aac");
  if (!codec)
    {
      std::cerr << "Failed to find the aac codec" << std::endl;
      std::exit(-1);
    }

  audioStream = avformat_new_stream(fmtCtx, codec);
  if (!audioStream)
    {
      std::cerr << "Failed to open audio stream" << std::endl;
      std::exit(-1);
    }

  audioCodecCtx = audioStream->codec;
  audioCodecCtx->bit_rate = lrintf(128000.0f);
  audioCodecCtx->sample_fmt = get_codec_sample_fmt(codec);
  audioCodecCtx->channel_layout = get_codec_channel_layout(codec);
  audioCodecCtx->sample_rate = AUDIO_RATE;
  audioCodecCtx->time_base = (AVRational) { 1, 1000 };
  audioCodecCtx->channels = av_get_channel_layout_nb_channels(audioCodecCtx->channel_layout);

  if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
    audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int err;
  if ((err = avcodec_open2(audioCodecCtx, codec, NULL)) < 0)
    {
      std::cerr << "(audio) avcodec_open2 failed " << err << std::endl;
      std::exit(-1);
    }

  swrCtx = swr_alloc();
  if (!swrCtx)
    {
      std::cerr << "Failed to allocate swr context" << std::endl;
      std::exit(-1);
    }

  av_opt_set_int(swrCtx, "in_sample_rate", AUDIO_RATE, 0);
  av_opt_set_int(swrCtx, "out_sample_rate", audioCodecCtx->sample_rate, 0);
  av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
  av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", audioCodecCtx->sample_fmt, 0);
  av_opt_set_channel_layout(swrCtx, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
  av_opt_set_channel_layout(swrCtx, "out_channel_layout", audioCodecCtx->channel_layout, 0);

  if (swr_init(swrCtx))
    {
      std::cerr << "Failed to initialize swr" << std::endl;
      std::exit(-1);
    }
}

void FrameWriter::init_codecs()
{
  init_video_stream();
  if (params.enable_audio)
    init_audio_stream();
  av_dump_format(fmtCtx, 0, params.file.c_str(), 1);
  if (avio_open(&fmtCtx->pb, params.file.c_str(), AVIO_FLAG_WRITE))
    {
      std::cerr << "avio_open failed" << std::endl;
      std::exit(-1);
    }
  AVDictionary *dummy = NULL;
  if (avformat_write_header(fmtCtx, &dummy) != 0)
    {
      std::cerr << "Failed to write file header" << std::endl;
      std::exit(-1);
    }
  av_dict_free(&dummy);
}


FrameWriter::FrameWriter(const FrameWriterParams& _params) :
  params(_params)
{
  if (params.enable_ffmpeg_debug_output)
    av_log_set_level(AV_LOG_DEBUG);
  else
    av_log_set_level(AV_LOG_WARNING);
  
  // Preparing the data concerning the format and codec,
  // in order to write properly the header, frame data and end of file.
  this->outputFmt = av_guess_format(NULL, params.file.c_str(), NULL);
  if (!outputFmt)
    {
      std::cerr << "Failed to guess output format for file " << params.file << std::endl;
      std::exit(-1);
    }

  if (avformat_alloc_output_context2(&this->fmtCtx,
                                     NULL,
                                     params.file_format.empty() ? NULL : params.file_format.c_str(),
                                     params.file.c_str()
                                     ) < 0)
    {
      std::cerr << "Failed to allocate output context" << std::endl;
      std::exit(-1);
    }

  init_codecs();
}

void FrameWriter::add_frame(const uint8_t* pixels, int64_t usec, bool y_invert)
{
  // Ignore y_invert! Can easily be done with a filter
  if (params.trace_video_progress) std::cerr << "TRACE: received input frame\n";
  int err;

  // Create a frame for the pixels
  AVFrame * frame = av_frame_alloc();
  frame->width       = params.width;
  frame->height      = params.height;
  frame->format      = get_input_format(); // a 32 bit RGBx pixel format
  frame->data[0]     = (uint8_t*) pixels;
  frame->linesize[0] = 4*params.width;
  frame->pts         = usec;  // because our time_base is US_RATIONAL

  if (y_invert) {
    // Do a cheap vflip using pointer manipulations.
    // Remark: This is also how the 'vflip' filter is operating
    //         but we cannot use 'vflip' because that invertion
    //         is not requested on-the-fly
    frame->data[0] += frame->linesize[0] * (frame->height-1);
    frame->linesize[0] = -frame->linesize[0];
  }

  
  // Is that needed? That makes sense for a RGB 'screencast'
  // but the documentation says that this is about the 'YUV range'
  // so this is probably ignored.
  if (true) {
    frame->color_range = AVCOL_RANGE_JPEG ;
  }


  // Push the RGB frame into the filtergraph */
  err = av_buffersrc_add_frame_flags(videoFilterSourceCtx, frame, 0);
  if (err < 0) {
    std::cerr << "Error while feeding the filtergraph\n";
    exit (-1);  
  }

  AVFrame *filtered_frame = av_frame_alloc();

  // filtered_frame->color_range = AVCOL_RANGE_JPEG;
  
  if (!filtered_frame) {
    std::cerr << "Error av_frame_alloc\n";
    exit (-1);  
  }
  
  // Pull filtered frames from the filtergraph 
  while (true) {

    err = av_buffersink_get_frame(videoFilterSinkCtx, filtered_frame);
    
    if (err==AVERROR(EAGAIN)) {
      // Not an error. No frame available.
      // Try again later.
      break;
    } else if (err==AVERROR_EOF) {
      // There will be no more output frames on this sink.
      // That could happen if a filter like 'trim' is used to
      // stop after a given time. 
      // If that happen, we need to inform the application
      // and it should (1) stop sending frames and (2) flush
      // the encoder.
      // TO BE TESTED
      std::cerr << "Got EOF in av_buffersink_get_frame\n";
      break;
    } else if (err<0) {
      av_frame_free(&filtered_frame);
      std::cerr << "Error in av_buffersink_get_frame\n";
      exit(-1);
    } 
      
    if (params.trace_video_progress) std::cerr << "TRACE: received filtered frame\n";
    
    // TODO: Is that necessary? This is done in the
    //       ffmpeg transcoding example but their input
    //       frames are produced by a decoder and so may
    //       have additional flags set. 
    filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;

    // printf("filtered color_range = %d\n", filtered_frame->color_range );
    // filtered_frame->color_range = AVCOL_RANGE_JPEG;
    // So we have a frame. Encode it!
    
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output=0;
    avcodec_encode_video2(videoCodecCtx, &pkt, filtered_frame, &got_output);

    av_frame_unref(filtered_frame);
    
    if (got_output)
      finish_frame(pkt, true);    
  }

  av_frame_free(&filtered_frame);
  av_frame_free(&frame);          
}

#define SRC_RATE 1e6
#define DST_RATE 1e3

static int64_t conv_audio_pts(SwrContext *ctx, int64_t in)
{
  int64_t d = (int64_t) AUDIO_RATE * AUDIO_RATE;

  /* Convert from audio_src_tb to 1/(src_samplerate * dst_samplerate) */
  in = av_rescale_rnd(in, d, SRC_RATE, AV_ROUND_NEAR_INF);

  /* In units of 1/(src_samplerate * dst_samplerate) */
  in = swr_next_pts(ctx, in);

  /* Convert from 1/(src_samplerate * dst_samplerate) to audio_dst_tb */
  return av_rescale_rnd(in, DST_RATE, d, AV_ROUND_NEAR_INF);
}

void FrameWriter::send_audio_pkt(AVFrame *frame)
{
  AVPacket pkt;
  av_init_packet(&pkt);
  pkt.data = NULL;
  pkt.size = 0;

  int got_output;
  avcodec_encode_audio2(audioCodecCtx, &pkt, frame, &got_output);
  if (got_output)
    finish_frame(pkt, false);
}

size_t FrameWriter::get_audio_buffer_size()
{
  return audioCodecCtx->frame_size << 3;
}

void FrameWriter::add_audio(const void* buffer)
{
  AVFrame *inputf = av_frame_alloc();
  inputf->sample_rate    = AUDIO_RATE;
  inputf->format         = AV_SAMPLE_FMT_FLT;
  inputf->channel_layout = AV_CH_LAYOUT_STEREO;
  inputf->nb_samples     = audioCodecCtx->frame_size;

  av_frame_get_buffer(inputf, 0);
  memcpy(inputf->data[0], buffer, get_audio_buffer_size());

  AVFrame *outputf = av_frame_alloc();
  outputf->format         = audioCodecCtx->sample_fmt;
  outputf->sample_rate    = audioCodecCtx->sample_rate;
  outputf->channel_layout = audioCodecCtx->channel_layout;
  outputf->nb_samples     = audioCodecCtx->frame_size;
  av_frame_get_buffer(outputf, 0);

  outputf->pts = conv_audio_pts(swrCtx, INT64_MIN);
  swr_convert_frame(swrCtx, outputf, inputf);

  send_audio_pkt(outputf);

  av_frame_free(&inputf);
  av_frame_free(&outputf);
}

void FrameWriter::finish_frame(AVPacket& pkt, bool is_video)
{
  static std::mutex fmt_mutex, pending_mutex;

  if (is_video)
    {
      if (params.trace_video_progress) std::cerr << "TRACE: received video packet\n";
      av_packet_rescale_ts(&pkt, vfilter.time_base, videoStream->time_base);
      pkt.stream_index = videoStream->index;
    } else
    {
      av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000 }, audioStream->time_base);
      pkt.stream_index = audioStream->index;
    }

  /* We use two locks to ensure that if WLOG the audio thread is waiting for
   * the video one, when the video becomes ready the audio thread will be the
   * next one to obtain the lock */
  if (params.enable_audio)
    {
      pending_mutex.lock();
      fmt_mutex.lock();
      pending_mutex.unlock();
    }

  av_interleaved_write_frame(fmtCtx, &pkt);
  av_packet_unref(&pkt);

  if (params.enable_audio)
    fmt_mutex.unlock();
}

FrameWriter::~FrameWriter()
{
  // Writing the delayed frames:
  AVPacket pkt;
  av_init_packet(&pkt);

  // TODO: Should also flush the filter graph but that 
  //       should be ione for now. 
  for (int got_output = 1; got_output;)
    {
      avcodec_encode_video2(videoCodecCtx, &pkt, NULL, &got_output);
      if (got_output)
        finish_frame(pkt, true);
    }

  for (int got_output = 1; got_output && params.enable_audio;)
    {
      avcodec_encode_audio2(audioCodecCtx, &pkt, NULL, &got_output);
      if (got_output)
        finish_frame(pkt, false);
    }

  // Writing the end of the file.
  av_write_trailer(fmtCtx);

  // Closing the file.
  if (!(outputFmt->flags & AVFMT_NOFILE))
    avio_closep(&fmtCtx->pb);
  avcodec_close(videoStream->codec);
  // Freeing all the allocated memory:
  av_frame_free(&encoder_frame);
  if (params.enable_audio)
    avcodec_close(audioStream->codec);
  // TODO: free all HW related stuffs.
  // TODO: free all Filter related stuffs.
  avformat_free_context(fmtCtx);
}
