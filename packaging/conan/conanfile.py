# Conan recipe for llbt.
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout


class LlbtConan(ConanFile):
    name = "llbt"
    version = "3.0.0"
    license = "Apache-2.0"
    url = "https://github.com/mjm918/llbt"
    description = ("Low Level Binary Tree - battle-tested files, mmap, locking, "
                   "crash-safe pages, and B+tree cursors.")
    settings = "os", "compiler", "build_type", "arch"
    options = {"encryption": [True, False]}
    default_options = {"encryption": False}
    exports_sources = ("CMakeLists.txt", "src/*", "include/*", "LICENSE", "NOTICE",
                       "THIRD-PARTY-NOTICES")

    def requirements(self):
        if self.options.encryption and self.settings.os not in ("Macos", "iOS"):
            self.requires("openssl/[>=3.0]")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["LLBT_BUILD_TESTS"] = False
        tc.variables["LLBT_BUILD_EXAMPLES"] = False
        tc.variables["LLBT_ENABLE_ENCRYPTION"] = bool(self.options.encryption)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["llbt-core"]
        self.cpp_info.set_property("cmake_target_name", "llbt::core")
        if self.settings.os in ("Macos", "iOS"):
            self.cpp_info.frameworks = ["Foundation", "CoreFoundation"]
