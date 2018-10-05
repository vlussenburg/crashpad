import os
import sys
import shutil

def copyfile(filepath) :
    dirname = os.path.dirname(os.path.realpath(filepath))
    shutil.copy(filepath, os.path.join(dirname, "libchromebase.a"))

if __name__ == '__main__' :
    copyfile("obj/third_party/mini_chromium/mini_chromium/base/libbase.a")

