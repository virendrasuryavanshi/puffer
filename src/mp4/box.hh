#ifndef BOX_HH
#define BOX_HH

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#include "mp4_file.hh"

namespace MP4 {

class Box
{
public:
  Box(const uint64_t size, const std::string & type);
  Box(const std::string & type);
  virtual ~Box() {}

  /* accessors */
  uint64_t size() { return size_; }
  std::string type() { return type_; }
  std::string raw_data() { return raw_data_; }

  /* parameter is a sink; use rvalue reference to save a "move" operation */
  void add_child(std::shared_ptr<Box> && child);

  std::vector<std::shared_ptr<Box>>::const_iterator children_begin();
  std::vector<std::shared_ptr<Box>>::const_iterator children_end();

  /* print the box and its children */
  virtual void print_box(const unsigned int indent = 0);

  /* parse the next 'data_size' bytes in 'mp4' */
  virtual void parse_data(MP4File & mp4, const uint64_t data_size);

  /* write the box and its children to 'mp4' */
  virtual void write_box(MP4File & mp4);

protected:
  void print_size_type(const unsigned int indent = 0);
  void write_size_type(MP4File & mp4);

  /* change 'size' to 'curr_offset - size_offset' and return */
  void fix_size_at(MP4File & mp4, const uint64_t size_offset);

  /* helper functions used in 'parse_data' */
  /* skip parsing the remaining data */
  void skip_data_left(MP4File & mp4, const uint64_t data_size,
                      const uint64_t init_offset);
  /* check no data remains to be parsed */
  void check_data_left(MP4File & mp4, const uint64_t data_size,
                       const uint64_t init_offset);

private:
  uint64_t size_;
  std::string type_;

  std::string raw_data_;
  std::vector<std::shared_ptr<Box>> children_;
};

class FullBox : public Box
{
public:
  FullBox(const uint64_t size, const std::string & type);
  FullBox(const std::string & type,
          const uint8_t version, const uint32_t flags);

  /* accessors */
  uint8_t version() { return version_; }
  uint32_t flags() { return flags_; }

protected:
  void print_version_flags(const unsigned int indent = 0);
  void parse_version_flags(MP4File & mp4);
  void write_version_flags(MP4File & mp4);

private:
  uint8_t version_;
  uint32_t flags_;
};

}

#endif /* BOX_HH */
