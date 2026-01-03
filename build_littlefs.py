Import("env")

# Simple and direct approach: replace the filesystem size parameter
# This is read by PlatformIO when building the LittleFS image

print("Forcing LittleFS size to 1376256 bytes (1344KB)")

# Set the filesystem size directly in the environment
env["FS_SIZE"] = 1376256

# Also update the board configuration
if "BOARD_CONFIG" in env:
    env["BOARD_CONFIG"]["build"]["filesystem_size"] = 1376256
