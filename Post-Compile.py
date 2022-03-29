#   For some strange reason, if the FinalBIN file does not exist yet,
#   You have to do a "Clean" before "Build" or it doesn't get copied...
#
#   If the file exists, it gets updated...
#
#   This is rather odd...

##############################################################
def walkItems(haystack, needle):
	nxt = False

	if isinstance(haystack, dict):
		for key, items in haystack.items():
			if nxt:
				return key

			elif isinstance(items, dict):
				result = walkItems(items, needle)
				if result != "":
					return result

			elif isinstance(items, list):
				result = walkItems(items, needle)
				if result != "":
					return result

			elif key == needle:
				nxt = True

		return ""

	elif isinstance(haystack, list):
		for key in haystack:
			if nxt:
				return key
			elif isinstance(key, dict):

				result = walkItems(key, needle)
				if result != "":
					return result

			elif isinstance(key, list):
				result = walkItems(key, needle)
				if result != "":
					return result

			elif key == needle:
				nxt = True

		return ""

	return ""
##############################################################

Import("env", "projenv")
from shutil import copyfile
import os

Import("env")

my_flags = env.ParseFlags(env['BUILD_FLAGS'])
my_cppdefines = my_flags.get("CPPDEFINES")

MyName = walkItems(my_cppdefines, 'DeviceName')
# MyType = walkItems(my_cppdefines, 'DeviceType')
MyFlash = walkItems(my_cppdefines, 'FlashSize')
MyVersion = walkItems(my_cppdefines, 'FIRMWAREVERSION')
MyBuild = env['PIOENV']

UNIXtime = env['UNIX_TIME']

BinaryDir = os.path.join(os.getcwd(), "binaries", MyVersion);
BinaryName = MyName + "_" + MyBuild + "_" + MyVersion + ".bin"

if not os.path.exists(BinaryDir):
	os.makedirs(BinaryDir)

BinaryPath = os.path.join(BinaryDir, BinaryName)

def Copy_Binary(*args, **kwargs):
    print("\n>---------- POST Compile BEGIN ----------<")

    print("Copying binary to distribution directory:\n", BinaryDir)

    target = str(kwargs['target'][0])
    copyfile(target, BinaryPath)

    FingerPrint = "[" + MyName + "|" + str(UNIXtime) + "|" + MyVersion + "|" + MyFlash + "MB]"

    file = open(BinaryPath, 'ab')
    file.write(bytearray(FingerPrint.encode()))
    file.close()

    FingerPrint = "\n" + FingerPrint + "\n"
    print("Fingerprinting:\n", FingerPrint)

    print(">---------- POST Compile Script END ----------<\n")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", Copy_Binary)   #post action for .hex
