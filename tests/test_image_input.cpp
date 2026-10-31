// ADR-017 "Image input" / ADR-031 §10a: image attached to an Adviser step.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>

#include <rapidjson/document.h>

#include "agentos/image_input.h"
#include "agentos/llm_proxy.h"

using namespace agentos;
namespace fs = std::filesystem;

namespace
{
  const std::string kPng ("\x89PNG\r\n\x1A\n\0\0\0\rIHDR", 16);
  const std::string kJpeg ("\xFF\xD8\xFF\xE0\0\x10JFIF", 10);
  const std::string kWebp ("RIFF\x24\0\0\0WEBPVP8 ", 16);

  fs::path write_tmp (const std::string &name, const std::string &bytes)
  {
    const fs::path p = fs::temp_directory_path () / ("agentos_img_" + name);
    std::ofstream (p, std::ios::binary) << bytes;
    return p;
  }

  LlmRequest image_request ()
  {
    LlmRequest req;
    req.model = "deepseek-flash";
    req.system_prompt = "sys";
    req.user_prompt = "describe";
    req.max_tokens = 100;
    req.image = LlmImage{"image/png", "QUJD"};
    return req;
  }
} // namespace

TEST (ImageInput, SniffsSupportedFormatsFromContent)
{
  EXPECT_EQ (sniff_image_media_type (kPng), "image/png");
  EXPECT_EQ (sniff_image_media_type (kJpeg), "image/jpeg");
  EXPECT_EQ (sniff_image_media_type (kWebp), "image/webp");
  EXPECT_EQ (sniff_image_media_type ("GIF89a......"), "image/gif");
  EXPECT_EQ (sniff_image_media_type ("GIF87a......"), "image/gif");
}

TEST (ImageInput, RejectsEverythingElse)
{
  EXPECT_FALSE (sniff_image_media_type (""));
  EXPECT_FALSE (sniff_image_media_type ("%PDF-1.7"));
  EXPECT_FALSE (sniff_image_media_type ("<svg xmlns="));
  EXPECT_FALSE (sniff_image_media_type ("<html>"));
  EXPECT_FALSE (sniff_image_media_type ("RIFF\x24\0\0\0WAVEfmt "));
  EXPECT_FALSE (sniff_image_media_type (std::string ("\x89PN", 3)));
}

TEST (ImageInput, Base64MatchesRfc4648Vectors)
{
  EXPECT_EQ (base64_encode (""), "");
  EXPECT_EQ (base64_encode ("f"), "Zg==");
  EXPECT_EQ (base64_encode ("fo"), "Zm8=");
  EXPECT_EQ (base64_encode ("foo"), "Zm9v");
  EXPECT_EQ (base64_encode ("foobar"), "Zm9vYmFy");
  EXPECT_EQ (base64_encode (std::string (1, '\0')), "AA==");
}

TEST (ImageInput, LoadEncodesAndSniffsByContentNotName)
{
  // A PNG named .jpg is still sent as image/png.
  const auto p = write_tmp ("misnamed.jpg", kPng);
  auto r = load_adviser_image (p);
  ASSERT_TRUE (r.ok) << r.error;
  EXPECT_EQ (r.value.media_type, "image/png");
  EXPECT_EQ (r.value.base64, base64_encode (kPng));
  fs::remove (p);
}

TEST (ImageInput, LoadRejectsMissingEmptyOversizedAndUnsupported)
{
  EXPECT_FALSE (load_adviser_image ("/nonexistent/agentos/img").ok);

  const auto empty = write_tmp ("empty", "");
  EXPECT_FALSE (load_adviser_image (empty).ok);

  const auto big = write_tmp ("big.png", kPng + std::string (100, 'x'));
  auto r = load_adviser_image (big, 64);
  EXPECT_FALSE (r.ok);
  EXPECT_NE (r.error.find ("limit"), std::string::npos);

  const auto svg = write_tmp ("x.png", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
  r = load_adviser_image (svg);
  EXPECT_FALSE (r.ok);
  EXPECT_NE (r.error.find ("unsupported"), std::string::npos);
  // The error never carries file content.
  EXPECT_EQ (r.error.find ("svg"), std::string::npos);

  fs::remove (empty);
  fs::remove (big);
  fs::remove (svg);
}

TEST (ImageInput, OpenAiBodyWithoutImageKeepsStringContent)
{
  LlmRequest req = image_request ();
  req.image.reset ();
  rapidjson::Document d;
  d.Parse (build_openai_body (req, true).c_str ());
  ASSERT_FALSE (d.HasParseError ());
  const auto &user = d["messages"][1];
  EXPECT_STREQ (user["role"].GetString (), "user");
  ASSERT_TRUE (user["content"].IsString ());
  EXPECT_STREQ (user["content"].GetString (), "describe");
}

TEST (ImageInput, OpenAiBodyCarriesImageInUserMessageOnly)
{
  rapidjson::Document d;
  d.Parse (build_openai_body (image_request (), true).c_str ());
  ASSERT_FALSE (d.HasParseError ());
  const auto &msgs = d["messages"];
  ASSERT_EQ (msgs.Size (), 2u);
  EXPECT_TRUE (msgs[0]["content"].IsString ()); // system stays text

  const auto &content = msgs[1]["content"];
  ASSERT_TRUE (content.IsArray ());
  ASSERT_EQ (content.Size (), 2u);
  EXPECT_STREQ (content[0]["type"].GetString (), "text");
  EXPECT_STREQ (content[0]["text"].GetString (), "describe");
  EXPECT_STREQ (content[1]["type"].GetString (), "image_url");
  EXPECT_STREQ (content[1]["image_url"]["url"].GetString (),
                "data:image/png;base64,QUJD");
}

TEST (ImageInput, AnthropicBodyCarriesBase64ImageBlock)
{
  rapidjson::Document d;
  d.Parse (build_anthropic_body (image_request ()).c_str ());
  ASSERT_FALSE (d.HasParseError ());
  const auto &content = d["messages"][0]["content"];
  ASSERT_TRUE (content.IsArray ());
  ASSERT_EQ (content.Size (), 2u);
  EXPECT_STREQ (content[0]["type"].GetString (), "image");
  EXPECT_STREQ (content[0]["source"]["type"].GetString (), "base64");
  EXPECT_STREQ (content[0]["source"]["media_type"].GetString (), "image/png");
  EXPECT_STREQ (content[0]["source"]["data"].GetString (), "QUJD");
  EXPECT_STREQ (content[1]["type"].GetString (), "text");
  EXPECT_STREQ (content[1]["text"].GetString (), "describe");
}

// Live check against DeepSeek, skipped without DEEPSEEK_API_KEY. Sends a
// 1x1 PNG to the vision model and expects a non-empty answer.
TEST (ImageInput, DeepSeekVisionE2E)
{
  const char *key = std::getenv ("DEEPSEEK_API_KEY");
  if (!key)
    GTEST_SKIP () << "DEEPSEEK_API_KEY not set";

  LlmProxy proxy (1, 120);
  LlmRequest req;
  req.base_url = "https://api.deepseek.com";
  req.api_key = key;
  req.model = "deepseek-flash";
  req.system_prompt = "You describe images.";
  req.user_prompt = "Describe this image in one sentence.";
  req.max_tokens = 200;
  req.image = LlmImage{"image/png",
                       "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQV"
                       "R42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg=="};

  auto fut = proxy.enqueue (req);
  ASSERT_EQ (fut.wait_for (std::chrono::seconds (120)),
             std::future_status::ready);
  auto res = fut.get ();
  ASSERT_TRUE (res.ok) << res.error;
  std::cout << "DeepSeek vision answer: '" << res.value.content << "'\n";
  EXPECT_FALSE (res.value.content.empty ());
}
