#pragma once
// CUM-495: head replies Z.ai (glm-5.3) actually returned on the host on
// 2026-10-10, with the device's request shape (response_format json_schema
// strict, ~16 KB head prompt): plain Markdown, no JSON, despite the schema.
// The first is verbatim (551 B); the second is the first bullet of the Arabic
// translation GLM returned inside a json_object turn - real model text used to
// cover multi-byte prose.
namespace harness_test {
inline const char* kZaiProseReply = R"ZAI(**Explanation of eddy covariance (eddy covariance method):**

- **What it measures:** the net exchange of carbon dioxide, water vapor, and heat between a given surface and the atmosphere.
- **How it works:** a three-dimensional sonic anemometer and a fast gas analyzer take measurements 10–20 times per second; computing the covariance between vertical wind and gas concentration over 30 minutes yields the flux value.
- **Why it matters:** it reveals whether an ecosystem is a carbon sink or a carbon source, continuously and without disturbing it.)ZAI";
inline const char* kZaiArabicBullet = R"ZAI(- **ماذا تقيس:** صافي تبادل ثاني أكسيد الكربون وبخار الماء والحرارة بين سطح ما والغلاف الجوي.)ZAI";
}  // namespace harness_test
