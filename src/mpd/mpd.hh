#ifndef TV_ENCODER_MPD_HH
#define TV_ENCODER_MPD_HH
#include <string>
#include <stack>
#include <sstream>
#include <memory>
#include <set>
#include <algorithm>
#include <iterator>
#include <chrono>

class XMLNode
{
public:
  const std::string tag_;
  bool hasContent;
  XMLNode(const std::string tag, bool hasContent): tag_(tag), hasContent(hasContent)
    { }
  XMLNode(const std::string tag): XMLNode(tag, false) { }
};

// based on
// https://gist.github.com/sebclaeys/1227644
// significant improvements are made
class XMLWriter
{
private:
  bool tag_open_;
  bool newline_;
  std::ostringstream os_;
  std::stack<XMLNode> elt_stack_;
  inline void close_tag();
  inline void indent();
  inline void write_escape(const std::string & str);

  const std::string xml_header = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
  const std::string xml_indent = "  ";

public:
  void open_elt(const std::string & tag);
  void close_elt();
  void close_all();

  void attr(const std::string & key, const std::string & val);
  void attr(const std::string & key, const unsigned int val);
  void attr(const std::string & key, const int val);

  void content(const int val);
  void content(const unsigned int val);
  void content(const std::string & val);

  std::string str();
  void output(std::ofstream &out);

  XMLWriter();
  ~XMLWriter();
};


namespace MPD {
enum class MimeType{ Video,  Audio_Webm, Audio_AAC };
const static uint8_t AvailableProfile[] {66, 88, 77, 100, 110, 122, 244, 44,
  83, 86, 128, 118, 138 };
struct Representation {
  std::string id;
  unsigned int bitrate;
  MimeType type;

  Representation(std::string id, unsigned int bitrate, MimeType type):
    id(id), bitrate(bitrate), type(type)
  {}

  virtual ~Representation() {}
};

struct VideoRepresentation: public Representation {
  unsigned int width;
  unsigned int height;
  uint8_t profile;
  unsigned int avc_level;
  float framerate;

  VideoRepresentation(std::string id, unsigned int width, unsigned int height,
      unsigned int bitrate, uint8_t profile, unsigned int avc_level,
      float framerate): Representation(id, bitrate, MimeType::Video),
  width(width), height(height), profile(profile),  avc_level(avc_level),
  framerate(framerate)
  {
    if (std::find(std::begin(AvailableProfile), std::end(AvailableProfile),
          profile) == std::end(AvailableProfile))
      throw std::runtime_error("Unsupported AVC profile.");
  }
};

struct AudioRepresentation: public Representation {
  unsigned int sampling_rate;

  AudioRepresentation(std::string id, unsigned int bitrate,
          unsigned int sampling_rate, bool use_opus): Representation(id,
              bitrate, use_opus? MimeType::Audio_Webm: MimeType::Audio_AAC),
          sampling_rate(sampling_rate)
  {}
};

inline bool operator<(const Representation & a, const Representation & b)
{
  return a.id < b.id;
}

class AdaptionSet {
public:
  int id;
  std::string init_uri;
  std::string media_uri;
  unsigned int duration; /* This needs to be determined from mp4 info */
  unsigned int timescale; /* this as well */

  AdaptionSet(int id, std::string init_uri, std::string media_uri,
      unsigned int duration, unsigned int timescale);

  virtual ~AdaptionSet() {}

};

class AudioAdaptionSet : public AdaptionSet {
public:
  void add_repr(std::shared_ptr<AudioRepresentation> repr);

  AudioAdaptionSet(int id, std::string init_uri, std::string media_uri,
    unsigned int duration, unsigned int timescale);

  std::set<std::shared_ptr<AudioRepresentation>> get_repr_set() const
  { return repr_set_; }

private:
  std::set<std::shared_ptr<AudioRepresentation>> repr_set_;
};

class VideoAdaptionSet : public AdaptionSet {
public:
  float framerate;

  VideoAdaptionSet(int id, std::string init_uri, std::string media_uri,
      float framerate, unsigned int duration, unsigned int timescale);

  void add_repr(std::shared_ptr<VideoRepresentation> repr);

  std::set<std::shared_ptr<VideoRepresentation>> get_repr_set() const
  { return repr_set_; }

private:
  std::set<std::shared_ptr<VideoRepresentation>> repr_set_;
};

inline bool operator<(const AdaptionSet & a, const AdaptionSet & b)
{
  return a.id < b.id;
}
}

class MPDWriter
{
public:
  MPDWriter(int64_t update_period, int64_t min_buffer_time,
          std::string base_url);
  ~MPDWriter();
  std::string flush();
  void add_video_adaption_set(std::shared_ptr<MPD::VideoAdaptionSet> set);
  void add_audio_adaption_set(std::shared_ptr<MPD::AudioAdaptionSet> set);
  void set_available_time(const std::chrono::seconds time)
  { availability_start_time_ = time; }

private:
  int64_t update_period_;
  int64_t min_buffer_time_;
  std::chrono::seconds availability_start_time_;
  std::unique_ptr<XMLWriter> writer_;
  std::string base_url_;
  std::set<std::shared_ptr<MPD::VideoAdaptionSet>> video_adaption_set_;
  std::set<std::shared_ptr<MPD::AudioAdaptionSet>> audio_adaption_set_;
  std::string format_time(const time_t time);
  std::string format_time(const std::chrono::seconds & time);
  std::string format_time_now();
  void write_adaption_set(std::shared_ptr<MPD::AdaptionSet>set);
  void write_video_adaption_set(std::shared_ptr<MPD::VideoAdaptionSet> set);
  void write_audio_adaption_set(std::shared_ptr<MPD::AudioAdaptionSet> set);
  std::string write_video_codec(std::shared_ptr<MPD::VideoRepresentation> repr);
  std::string write_audio_codec(std::shared_ptr<MPD::AudioRepresentation> repr);
  void write_repr(std::shared_ptr<MPD::Representation> repr);
  void write_video_repr(std::shared_ptr<MPD::VideoRepresentation> repr);
  void write_audio_repr(std::shared_ptr<MPD::AudioRepresentation> repr);
  std::string convert_pt(unsigned int seconds);
  void write_framerate(const float & framerate);
};

#endif /* TV_ENCODER_MPD_HH */