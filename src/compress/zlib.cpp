#include <fc/compress/zlib.hpp>
#include <fc/exception/exception.hpp>

#include "miniz.c"

namespace fc
{
  std::string zlib_compress(const std::string& in)
  {
    size_t compressed_message_length;
    char* compressed_message = (char*)tdefl_compress_mem_to_heap(in.c_str(), in.size(), &compressed_message_length,  TDEFL_WRITE_ZLIB_HEADER | TDEFL_DEFAULT_MAX_PROBES);
    std::string result(compressed_message, compressed_message_length);
    free(compressed_message);
    return result;
  }

  std::string zlib_decompress(const std::string& in)
  {
    size_t decompressed_length;
    char* decompressed = (char*)tinfl_decompress_mem_to_heap(in.data(), in.size(), &decompressed_length, TINFL_FLAG_PARSE_ZLIB_HEADER);
    FC_ASSERT(decompressed != nullptr, "zlib decompression failed");
    std::string result(decompressed, decompressed_length);
    free(decompressed);
    return result;
  }
}
