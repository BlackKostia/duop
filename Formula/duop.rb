class Duop < Formula
  desc "Ultra-fast zero-libc dual-pane TUI file manager (static ~250 KB ELF)"
  homepage "https://github.com/BlackKostia/duop"
  url "https://github.com/BlackKostia/duop/archive/refs/tags/v2.11.0.tar.gz"
  # Fill in after the v2.11.0 tag is published:
  #   curl -sL https://github.com/BlackKostia/duop/archive/refs/tags/v2.11.0.tar.gz | sha256sum
  # sha256 "<sha256>"
  license "MIT"
  head "https://github.com/BlackKostia/duop.git", branch: "main"

  depends_on "make" => :build
  depends_on "python@3" => :build
  depends_on :linux

  on_intel do
    depends_on "binutils" => :build
    depends_on "gcc" => :build
  end

  on_arm do
    depends_on "binutils" => :build
    depends_on "llvm" => :build
  end

  def install
    if Hardware::CPU.arm?
      system "make", "release", "ARCH=aarch64", "AS=as", "LD=ld"
    else
      system "make", "release", "ARCH=x86_64"
    end
    bin.install "duop"
    pkgshare.install "scripts/duop-cd.sh"
    (pkgshare/"layouts").install Dir["layouts/*.layout"]
    (pkgshare/"lang").install Dir["lang/*.lang"]
  end

  test do
    assert_match "Duop", shell_output("#{bin}/duop --version")
  end
end
