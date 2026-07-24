require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "ProductAnalyzerPlugin"
  s.version      = package["version"]
  s.summary      = "VisionCamera C++ frame processor plugin: OpenCV contour centroid + sensor-fusion pitch/roll"
  s.homepage     = "https://example.com"
  s.license      = "MIT"
  s.authors      = { "VisionApp" => "andrei.chirata2006@gmail.com" }
  s.platforms    = { :ios => "15.1" }
  s.source       = { :path => "." }

  s.source_files = [
    "ios/ProductAnalyzerPlugin/**/*.{h,m,mm}",
    "cpp/**/*.{h,hpp,cpp}",
  ]

  s.frameworks = ["CoreMotion", "CoreMedia", "CoreVideo"]

  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "HEADER_SEARCH_PATHS" => "\"$(PODS_TARGET_SRCROOT)/cpp\"",
  }

  s.dependency "VisionCamera"
  s.dependency "OpenCV", "~> 4.3"
end
