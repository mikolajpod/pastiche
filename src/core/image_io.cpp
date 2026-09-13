#include "image_io.hpp"
#include "exif.hpp"
#include "fs.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <vector>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#ifdef HAVE_JXL
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/color_encoding.h>
#endif

namespace pastiche {

ImageFormat detect_format(const uint8_t* d, size_t n)
{
    if (!d || n < 12) return ImageFormat::Unknown;
    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return ImageFormat::JPEG;
    if (std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageFormat::PNG;
    if (std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return ImageFormat::WebP;
    if (d[0] == 0xFF && d[1] == 0x0A) return ImageFormat::JXL;                       // bare codestream
    if (std::memcmp(d, "\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a", 12) == 0) return ImageFormat::JXL;  // container
    return ImageFormat::Unknown;
}

const char* format_name(ImageFormat f)
{
    switch (f) {
        case ImageFormat::JPEG: return "JPEG";
        case ImageFormat::PNG: return "PNG";
        case ImageFormat::WebP: return "WebP";
        case ImageFormat::JXL: return "JPEG XL";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// JPEG (libjpeg-turbo)
// ---------------------------------------------------------------------------
namespace {

struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jmp;
    char msg[JMSG_LENGTH_MAX];
};

void jpeg_error_exit(j_common_ptr cinfo)
{
    JpegErr* e = reinterpret_cast<JpegErr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jmp, 1);
}

void jpeg_silent(j_common_ptr) {}

std::string decode_jpeg(const uint8_t* data, size_t size, Image& out)
{
    jpeg_decompress_struct cinfo;
    JpegErr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_error_exit;
    err.pub.output_message = jpeg_silent;
    err.msg[0] = '\0';
    if (setjmp(err.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return std::string("JPEG decode failed: ") + err.msg;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return "JPEG decode failed: unsupported colour space";
    }
    out = Image(static_cast<int>(cinfo.output_width), static_cast<int>(cinfo.output_height), 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = out.row(static_cast<int>(cinfo.output_scanline));
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return {};
}

// ---------------------------------------------------------------------------
// PNG (libpng simplified API for reading, classic API for writing)
// ---------------------------------------------------------------------------
std::string decode_png(const uint8_t* data, size_t size, Image& out)
{
    png_image image;
    std::memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, size))
        return std::string("PNG decode failed: ") + image.message;
    const bool alpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    image.format = alpha ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;
    out = Image(static_cast<int>(image.width), static_cast<int>(image.height), alpha ? 4 : 3);
    if (!png_image_finish_read(&image, nullptr, out.data.data(), 0, nullptr)) {
        std::string msg = std::string("PNG decode failed: ") + image.message;
        png_image_free(&image);
        out = Image();
        return msg;
    }
    return {};
}

// ---------------------------------------------------------------------------
// WebP (libwebp)
// ---------------------------------------------------------------------------
std::string decode_webp(const uint8_t* data, size_t size, Image& out)
{
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(data, size, &f) != VP8_STATUS_OK)
        return "WebP decode failed: invalid header";
    const int c = f.has_alpha ? 4 : 3;
    out = Image(f.width, f.height, c);
    const size_t stride = out.row_bytes();
    uint8_t* ok = f.has_alpha
        ? WebPDecodeRGBAInto(data, size, out.data.data(), out.data.size(), static_cast<int>(stride))
        : WebPDecodeRGBInto(data, size, out.data.data(), out.data.size(), static_cast<int>(stride));
    if (!ok) { out = Image(); return "WebP decode failed"; }
    return {};
}

// ---------------------------------------------------------------------------
// JPEG XL (libjxl, optional)
// ---------------------------------------------------------------------------
#ifdef HAVE_JXL
std::string decode_jxl(const uint8_t* data, size_t size, Image& out)
{
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) return "JXL decode failed: JxlDecoderCreate";
    std::string err;
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return "JXL decode failed: SubscribeEvents";
    }
    JxlDecoderSetInput(dec, data, size);
    JxlDecoderCloseInput(dec);
    JxlPixelFormat fmt = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_ERROR) { err = "JXL decode failed: corrupt data"; break; }
        if (st == JXL_DEC_NEED_MORE_INPUT) { err = "JXL decode failed: truncated file"; break; }
        if (st == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) { err = "JXL decode failed: basic info"; break; }
            fmt.num_channels = info.alpha_bits > 0 ? 4 : 3;
            out = Image(static_cast<int>(info.xsize), static_cast<int>(info.ysize), static_cast<int>(fmt.num_channels));
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t need = 0;
            if (JxlDecoderImageOutBufferSize(dec, &fmt, &need) != JXL_DEC_SUCCESS || need != out.data.size()) {
                err = "JXL decode failed: buffer size mismatch"; break;
            }
            if (JxlDecoderSetImageOutBuffer(dec, &fmt, out.data.data(), need) != JXL_DEC_SUCCESS) {
                err = "JXL decode failed: SetImageOutBuffer"; break;
            }
        } else if (st == JXL_DEC_FULL_IMAGE) {
            // one frame is enough
        } else if (st == JXL_DEC_SUCCESS) {
            break;
        }
    }
    JxlDecoderDestroy(dec);
    if (!err.empty()) out = Image();
    return err;
}
#endif

} // namespace

std::string decode_image(const uint8_t* data, size_t size, Image& out)
{
    out = Image();
    switch (detect_format(data, size)) {
        case ImageFormat::JPEG: {
            const std::string e = decode_jpeg(data, size, out);
            if (!e.empty()) return e;
            const int o = jpeg_exif_orientation(data, size);
            if (o > 1) out = apply_orientation(out, o);
            return {};
        }
        case ImageFormat::PNG: return decode_png(data, size, out);
        case ImageFormat::WebP: return decode_webp(data, size, out);
        case ImageFormat::JXL:
#ifdef HAVE_JXL
            return decode_jxl(data, size, out);
#else
            return "JPEG XL input requires a build with libjxl";
#endif
        default:
            return "unsupported image format (expected " + supported_input_formats() + ")";
    }
}

std::string load_image(const std::string& path, Image& out)
{
    std::vector<uint8_t> buf;
    const std::string e = read_file(path, buf);
    if (!e.empty()) return e;
    const std::string d = decode_image(buf.data(), buf.size(), out);
    if (!d.empty()) return d + ": " + path;
    return {};
}

std::string save_png(const std::string& path, const Image& img)
{
    if (img.empty() || (img.channels != 3 && img.channels != 4)) return "save_png: empty or unsupported image";
    FILE* fp = fopen_utf8(path, "wb");
    if (!fp) return "cannot open '" + path + "' for writing";

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return "png_create_write_struct failed"; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); std::fclose(fp); return "png_create_info_struct failed"; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        std::remove(path.c_str());
        return "PNG write error (libpng)";
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, static_cast<png_uint_32>(img.width), static_cast<png_uint_32>(img.height), 8,
                 img.channels == 4 ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < img.height; ++y)
        png_write_row(png, const_cast<png_bytep>(img.row(y)));
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    if (std::fclose(fp) != 0) {
        std::remove(path.c_str());
        return "write error while flushing '" + path + "' (disk full?)";
    }
    return {};
}

#ifdef HAVE_JXL
std::string save_jxl(const std::string& path, const Image& img)
{
    if (img.empty() || (img.channels != 3 && img.channels != 4)) return "save_jxl: empty or unsupported image";
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    if (!enc) return "JxlEncoderCreate failed";
    auto fail = [&](const char* what) { JxlEncoderDestroy(enc); return std::string(what) + " failed"; };

    const bool alpha = img.channels == 4;
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = static_cast<uint32_t>(img.width);
    bi.ysize = static_cast<uint32_t>(img.height);
    bi.bits_per_sample = 8;
    bi.exponent_bits_per_sample = 0;
    bi.num_color_channels = 3;
    bi.alpha_bits = alpha ? 8 : 0;
    bi.alpha_exponent_bits = 0;
    bi.num_extra_channels = alpha ? 1 : 0;
    bi.uses_original_profile = JXL_TRUE;
    if (JxlEncoderSetBasicInfo(enc, &bi) != JXL_ENC_SUCCESS) return fail("JxlEncoderSetBasicInfo");
    if (alpha) {
        JxlExtraChannelInfo eci;
        JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &eci);
        eci.bits_per_sample = 8;
        eci.exponent_bits_per_sample = 0;
        if (JxlEncoderSetExtraChannelInfo(enc, 0, &eci) != JXL_ENC_SUCCESS) return fail("JxlEncoderSetExtraChannelInfo");
    }
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc, &color) != JXL_ENC_SUCCESS) return fail("JxlEncoderSetColorEncoding");

    JxlEncoderFrameSettings* opts = JxlEncoderFrameSettingsCreate(enc, nullptr);
    if (!opts) return fail("JxlEncoderFrameSettingsCreate");
    if (JxlEncoderSetFrameLossless(opts, JXL_TRUE) != JXL_ENC_SUCCESS) return fail("JxlEncoderSetFrameLossless");

    JxlPixelFormat fmt = {static_cast<uint32_t>(img.channels), JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(opts, &fmt, img.data.data(), img.data.size()) != JXL_ENC_SUCCESS)
        return fail("JxlEncoderAddImageFrame");
    JxlEncoderCloseInput(enc);

    std::vector<uint8_t> output(1 << 16);
    uint8_t* next_out = output.data();
    size_t avail_out = output.size();
    JxlEncoderStatus status;
    while ((status = JxlEncoderProcessOutput(enc, &next_out, &avail_out)) == JXL_ENC_NEED_MORE_OUTPUT) {
        const size_t used = static_cast<size_t>(next_out - output.data());
        output.resize(output.size() * 2);
        next_out = output.data() + used;
        avail_out = output.size() - used;
    }
    JxlEncoderDestroy(enc);
    if (status != JXL_ENC_SUCCESS) return "JxlEncoderProcessOutput failed";
    output.resize(static_cast<size_t>(next_out - output.data()));
    return write_file(path, output.data(), output.size());
}
#else
std::string save_jxl(const std::string&, const Image&)
{
    return "JPEG XL output requires a build with libjxl";
}
#endif

std::string save_image(const std::string& path, const Image& img)
{
    const std::string ext = path_extension(path);
    if (ext == ".png") return save_png(path, img);
    if (ext == ".jxl") return save_jxl(path, img);
    return "unsupported output extension '" + ext + "' (expected " + supported_output_formats() + ")";
}

bool jxl_available()
{
#ifdef HAVE_JXL
    return true;
#else
    return false;
#endif
}

std::string supported_input_formats()
{
    return jxl_available() ? "JPEG, PNG, WebP, JPEG XL" : "JPEG, PNG, WebP";
}

std::string supported_output_formats()
{
    return jxl_available() ? ".png, .jxl" : ".png";
}

std::string supported_input_extensions()
{
    // jfif/jfi/jpe/jif are all plain JPEG; Windows browsers in particular like
    // to save downloads as .jfif.
    std::string ext = "jpg,jpeg,jfif,jfi,jpe,jif,png,webp";
    if (jxl_available()) ext += ",jxl";
    return ext;
}

} // namespace pastiche
