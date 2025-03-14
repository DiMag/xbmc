/*
 *      Copyright (C) 2005-2014 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifdef TARGET_WINDOWS
#include <sys/types.h>
#define __STDC_FORMAT_MACROS
#include <cinttypes>
#define platform_stricmp _stricmp
#else
#include <inttypes.h>
#define platform_stricmp strcasecmp
#endif
#include <cerrno>
#include <dirent.h>
#include <map>

#include "guilib/XBTF.h"
#include "guilib/XBTFReader.h"

#include "DecoderManager.h"

#include "XBTFWriter.h"
#include "md5.h"
#include "cmdlineargs.h"

#ifdef TARGET_WINDOWS
#define strncasecmp _strnicmp
#endif

#include <vector>

#include <lzo/lzo1x.h>
#include <sys/stat.h>

#define FLAGS_USE_LZO     1

#define DIR_SEPARATOR '/'

namespace
{

const char* GetFormatString(KD_TEX_FMT format)
{
  switch (format)
  {
    case KD_TEX_FMT_SDR_R8:
      return "R8   ";
    case KD_TEX_FMT_SDR_RG8:
      return "RG8  ";
    case KD_TEX_FMT_SDR_RGBA8:
      return "RGBA8";
    case KD_TEX_FMT_SDR_BGRA8:
      return "BGRA8";
    default:
      return "?????";
  }
}

void Usage()
{
  puts("Texture Packer Version 3");
  puts("");
  puts("Tool to pack XBT 3 texture files, used in Kodi Piers (v22).");
  puts("Accepts the following file formats as input: PNG (preferred), JPG and GIF.");
  puts("");
  puts("Usage:");
  puts("  -help            Show this screen.");
  puts("  -input <dir>     Input directory. Default: current dir");
  puts("  -output <dir>    Output directory/filename. Default: Textures.xbt");
  puts("  -dupecheck       Enable duplicate file detection. Reduces output file size. Default: off");
}

} // namespace

class TexturePacker
{
public:
  TexturePacker() = default;
  ~TexturePacker() = default;

  void EnableDupeCheck() { m_dupecheck = true; }

  void EnableVerboseOutput() { decoderManager.EnableVerboseOutput(); }

  int createBundle(const std::string& InputDir, const std::string& OutputFile);

  void SetFlags(unsigned int flags) { m_flags = flags; }

private:
  void CreateSkeletonHeader(CXBTFWriter& xbtfWriter,
                            const std::string& fullPath,
                            const std::string& relativePath = "");

  CXBTFFrame CreateXBTFFrame(DecodedFrame& decodedFrame, CXBTFWriter& writer) const;

  bool CheckDupe(MD5Context* ctx, unsigned int pos);

  void ConvertToSingleChannel(RGBAImage& image, uint32_t channel);
  void ConvertToDualChannel(RGBAImage& image);
  void ReduceChannels(RGBAImage& image);

  DecoderManager decoderManager;

  std::map<std::string, unsigned int> m_hashes;
  std::vector<unsigned int> m_dupes;

  bool m_dupecheck{false};
  unsigned int m_flags{0};
};

void TexturePacker::CreateSkeletonHeader(CXBTFWriter& xbtfWriter,
                                         const std::string& fullPath,
                                         const std::string& relativePath)
{
  struct dirent* dp;
  struct stat stat_p;
  DIR *dirp = opendir(fullPath.c_str());

  if (dirp)
  {
    for (errno = 0; (dp = readdir(dirp)); errno = 0)
    {
      if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
      {
        continue;
      }

      //stat to check for dir type (reiserfs fix)
      std::string fileN = fullPath + "/" + dp->d_name;
      if (stat(fileN.c_str(), &stat_p) == 0)
      {
        if (dp->d_type == DT_DIR || stat_p.st_mode & S_IFDIR)
        {
          std::string tmpPath = relativePath;
          if (tmpPath.size() > 0)
          {
            tmpPath += "/";
          }

          CreateSkeletonHeader(xbtfWriter, fullPath + DIR_SEPARATOR + dp->d_name,
                               tmpPath + dp->d_name);
        }
        else if (decoderManager.IsSupportedGraphicsFile(dp->d_name))
        {
          std::string fileName = "";
          if (relativePath.size() > 0)
          {
            fileName += relativePath;
            fileName += "/";
          }

          fileName += dp->d_name;

          CXBTFFile file;
          file.SetPath(fileName);
          xbtfWriter.AddFile(file);
        }
      }
    }
    if (errno)
      fprintf(stderr, "Error reading directory %s (%s)\n", fullPath.c_str(), strerror(errno));

    closedir(dirp);
  }
  else
  {
    fprintf(stderr, "Error opening %s (%s)\n", fullPath.c_str(), strerror(errno));
  }
}

CXBTFFrame TexturePacker::CreateXBTFFrame(DecodedFrame& decodedFrame, CXBTFWriter& writer) const
{
  const unsigned int delay = decodedFrame.delay;
  const unsigned int width = decodedFrame.rgbaImage.width;
  const unsigned int height = decodedFrame.rgbaImage.height;
  const uint32_t bpp = decodedFrame.rgbaImage.bbp;
  const unsigned int size = width * height * (bpp / 8);
  const uint32_t format = static_cast<uint32_t>(decodedFrame.rgbaImage.textureFormat) |
                          static_cast<uint32_t>(decodedFrame.rgbaImage.textureAlpha) |
                          static_cast<uint32_t>(decodedFrame.rgbaImage.textureSwizzle);
  unsigned char* data = (unsigned char*)decodedFrame.rgbaImage.pixels.data();

  CXBTFFrame frame;
  lzo_uint packedSize = size;

  if ((m_flags & FLAGS_USE_LZO) == FLAGS_USE_LZO)
  {
    // grab a temporary buffer for unpacking into
    packedSize = size + size / 16 + 64 + 3; // see simple.c in lzo

    std::vector<uint8_t> packed;
    packed.resize(packedSize);

    std::vector<uint8_t> working;
    working.resize(LZO1X_999_MEM_COMPRESS);

    if (lzo1x_999_compress(data, size, packed.data(), &packedSize, working.data()) != LZO_E_OK ||
        packedSize > size)
    {
      // compression failed, or compressed size is bigger than uncompressed, so store as uncompressed
      packedSize = size;
      writer.AppendContent(data, size);
    }
    else
    { // success
      lzo_uint optimSize = size;
      if (lzo1x_optimize(packed.data(), packedSize, data, &optimSize, NULL) != LZO_E_OK ||
          optimSize != size)
      { //optimisation failed
        packedSize = size;
        writer.AppendContent(data, size);
      }
      else
      { // success
        writer.AppendContent(packed.data(), packedSize);
      }
    }
  }
  else
  {
    writer.AppendContent(data, size);
  }
  frame.SetPackedSize(packedSize);
  frame.SetUnpackedSize(size);
  frame.SetWidth(width);
  frame.SetHeight(height);
  frame.SetFormat(format);
  frame.SetDuration(delay);
  return frame;
}

bool TexturePacker::CheckDupe(MD5Context* ctx,
                              unsigned int pos)
{
  unsigned char digest[17];
  MD5Final(digest,ctx);
  digest[16] = 0;
  char hex[33];
  snprintf(hex, sizeof(hex),
           "%02X%02X%02X%02X%02X%02X%02X%02X"
           "%02X%02X%02X%02X%02X%02X%02X%02X",
           digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
           digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14],
           digest[15]);
  hex[32] = 0;
  std::map<std::string, unsigned int>::iterator it = m_hashes.find(hex);
  if (it != m_hashes.end())
  {
    m_dupes[pos] = it->second;
    return true;
  }

  m_hashes[hex] = pos;
  m_dupes[pos] = pos;

  return false;
}

void TexturePacker::ConvertToSingleChannel(RGBAImage& image, uint32_t channel)
{
  uint32_t size = (image.width * image.height);
  for (uint32_t i = 0; i < size; i++)
  {
    image.pixels[i] = image.pixels[i * 4 + channel];
  }

  image.textureFormat = KD_TEX_FMT_SDR_R8;

  image.bbp = 8;
  image.pitch = 1 * image.width;
}

void TexturePacker::ConvertToDualChannel(RGBAImage& image)
{
  uint32_t size = (image.width * image.height);
  for (uint32_t i = 0; i < size; i++)
  {
    image.pixels[i * 2] = image.pixels[i * 4];
    image.pixels[i * 2 + 1] = image.pixels[i * 4 + 3];
  }
  image.textureFormat = KD_TEX_FMT_SDR_RG8;
  image.bbp = 16;
  image.pitch = 2 * image.width;
}

void TexturePacker::ReduceChannels(RGBAImage& image)
{
  if (image.textureFormat != KD_TEX_FMT_SDR_BGRA8)
    return;

  uint32_t size = (image.width * image.height);
  uint8_t red = image.pixels[0];
  uint8_t green = image.pixels[1];
  uint8_t blue = image.pixels[2];
  uint8_t alpha = image.pixels[3];
  bool uniformRed = true;
  bool uniformGreen = true;
  bool uniformBlue = true;
  bool uniformAlpha = true;
  bool isGrey = true;
  bool isIntensity = true;

  // Checks each pixel for various properties.
  for (uint32_t i = 0; i < size; i++)
  {
    if (image.pixels[i * 4] != red)
      uniformRed = false;
    if (image.pixels[i * 4 + 1] != green)
      uniformGreen = false;
    if (image.pixels[i * 4 + 2] != blue)
      uniformBlue = false;
    if (image.pixels[i * 4 + 3] != alpha)
      uniformAlpha = false;
    if (image.pixels[i * 4] != image.pixels[i * 4 + 1] ||
        image.pixels[i * 4] != image.pixels[i * 4 + 2])
      isGrey = false;
    if (image.pixels[i * 4] != image.pixels[i * 4 + 1] ||
        image.pixels[i * 4] != image.pixels[i * 4 + 2] ||
        image.pixels[i * 4] != image.pixels[i * 4 + 3])
      isIntensity = false;
  }

  if (uniformAlpha && alpha != 0xff)
    printf("WARNING: uniform alpha detected! Consider using diffusecolor!\n");

  bool isWhite = red == 0xff && green == 0xff && blue == 0xff;
  if (uniformRed && uniformGreen && uniformBlue && !isWhite)
    printf("WARNING: uniform color detected! Consider using diffusecolor!\n");

  if (uniformAlpha && alpha == 0xff)
  {
    // we have a opaque texture, L or RGBX
    if (isGrey)
    {
      ConvertToSingleChannel(image, 1);
      image.textureSwizzle = KD_TEX_SWIZ_RRR1;
    }
    image.textureAlpha = KD_TEX_ALPHA_OPAQUE;
  }
  else if (uniformRed && uniformGreen && uniformBlue && isWhite)
  {
    // an alpha only texture
    ConvertToSingleChannel(image, 3);
    image.textureSwizzle = KD_TEX_SWIZ_111R;
  }
  else if (isIntensity)
  {
    // this is an intensity (GL_INTENSITY) texture
    ConvertToSingleChannel(image, 0);
    image.textureSwizzle = KD_TEX_SWIZ_RRRR;
  }
  else if (isGrey)
  {
    // a LA texture
    ConvertToDualChannel(image);
    image.textureSwizzle = KD_TEX_SWIZ_RRRG;
  }
  else
  {
    // BGRA
  }
}

int TexturePacker::createBundle(const std::string& InputDir, const std::string& OutputFile)
{
  CXBTFWriter writer(OutputFile);
  if (!writer.Create())
  {
    fprintf(stderr, "Error creating file\n");
    return 1;
  }

  CreateSkeletonHeader(writer, InputDir);

  std::vector<CXBTFFile> files = writer.GetFiles();
  m_dupes.resize(files.size());

  for (size_t i = 0; i < files.size(); i++)
  {
    struct MD5Context ctx;
    MD5Init(&ctx);
    CXBTFFile& file = files[i];

    std::string fullPath = InputDir;
    fullPath += file.GetPath();

    std::string output = file.GetPath();

    DecodedFrames frames;
    bool loaded = decoderManager.LoadFile(fullPath, frames);

    if (!loaded)
    {
      fprintf(stderr, "...unable to load image %s\n", file.GetPath().c_str());
      continue;
    }

    for (unsigned int j = 0; j < frames.frameList.size(); j++)
      ReduceChannels(frames.frameList[j].rgbaImage);

    printf("%s\n", output.c_str());
    bool skip=false;
    if (m_dupecheck)
    {
      for (unsigned int j = 0; j < frames.frameList.size(); j++)
        MD5Update(&ctx, (const uint8_t*)frames.frameList[j].rgbaImage.pixels.data(),
                  frames.frameList[j].rgbaImage.height * frames.frameList[j].rgbaImage.pitch);

      if (CheckDupe(&ctx, i))
      {
        printf("****  duplicate of %s\n", files[m_dupes[i]].GetPath().c_str());
        file.GetFrames().insert(file.GetFrames().end(),
                                files[m_dupes[i]].GetFrames().begin(),
                                files[m_dupes[i]].GetFrames().end());
        skip = true;
      }
    }
    else
    {
      m_dupes[i] = i;
    }

    if (!skip)
    {
      for (unsigned int j = 0; j < frames.frameList.size(); j++)
      {
        CXBTFFrame frame = CreateXBTFFrame(frames.frameList[j], writer);
        file.GetFrames().push_back(frame);
        printf("    frame %4i (delay:%4i)                         %s%c (%d,%d @ %" PRIu64
               " bytes)\n",
               j, frame.GetDuration(), GetFormatString(frame.GetKDFormat()),
               frame.HasAlpha() ? ' ' : '*', frame.GetWidth(), frame.GetHeight(),
               frame.GetUnpackedSize());
      }
    }
    file.SetLoop(0);

    writer.UpdateFile(file);
  }

  if (!writer.UpdateHeader(m_dupes))
  {
    fprintf(stderr, "Error writing header to file\n");
    return 1;
  }

  if (!writer.Close())
  {
    fprintf(stderr, "Error closing file\n");
    return 1;
  }

  return 0;
}

int main(int argc, char* argv[])
{
  if (lzo_init() != LZO_E_OK)
    return 1;
  bool valid = false;

  CmdLineArgs args(argc, (const char**)argv);

  if (args.size() == 1)
  {
    Usage();
    return 1;
  }

  std::string InputDir;
  std::string OutputFilename = "Textures.xbt";

  TexturePacker texturePacker;

  texturePacker.SetFlags(FLAGS_USE_LZO);

  for (unsigned int i = 1; i < args.size(); ++i)
  {
    if (!platform_stricmp(args[i], "-help") || !platform_stricmp(args[i], "-h") || !platform_stricmp(args[i], "-?"))
    {
      Usage();
      return 1;
    }
    else if (!platform_stricmp(args[i], "-input") || !platform_stricmp(args[i], "-i"))
    {
      InputDir = args[++i];
      valid = true;
    }
    else if (!strcmp(args[i], "-dupecheck"))
    {
      texturePacker.EnableDupeCheck();
    }
    else if (!strcmp(args[i], "-verbose"))
    {
      texturePacker.EnableVerboseOutput();
    }
    else if (!platform_stricmp(args[i], "-output") || !platform_stricmp(args[i], "-o"))
    {
      OutputFilename = args[++i];
      valid = true;
#ifdef TARGET_POSIX
      char *c = NULL;
      while ((c = (char *)strchr(OutputFilename.c_str(), '\\')) != NULL) *c = '/';
#endif
    }
    else
    {
      fprintf(stderr, "Unrecognized command line flag: %s\n", args[i]);
    }
  }

  if (!valid)
  {
    Usage();
    return 1;
  }

  size_t pos = InputDir.find_last_of(DIR_SEPARATOR);
  if (pos != InputDir.length() - 1)
    InputDir += DIR_SEPARATOR;

  texturePacker.createBundle(InputDir, OutputFilename);
}
