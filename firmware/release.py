import sys
import os
import json
import zipfile
from datetime import datetime

# 切换到项目根目录
os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def get_board_type():
    with open("build/compile_commands.json") as f:
        data = json.load(f)
        for item in data:
            if not item["file"].endswith("main.cc"):
                continue
            command = item["command"]
            # extract -DBOARD_TYPE=xxx
            board_type = command.split("-DBOARD_TYPE=\\\"")[1].split("\\\"")[0].strip()
            return board_type
    return None

def get_project_version():
    with open("CMakeLists.txt") as f:
        for line in f:
            if line.startswith("set(PROJECT_VER"):
                return line.split("\"")[1].split("\"")[0].strip()
    return None

def merge_bin():
    if os.system("idf.py merge-bin") != 0:
        print("merge bin failed")
        sys.exit(1)
    
    if os.path.exists("licence_bin.py"):
        if os.system("python licence_bin.py") != 0:
            print("licence bin failed")
            sys.exit(1)

def zip_bin(board_type, project_version, path):
    
    curr_time = datetime.now().strftime("%Y%m%d%H%M")
    
    name = f"{board_type}_v{project_version}_{curr_time}"
    
    if not os.path.exists(f"{path}/releases"):
        os.makedirs(f"{path}/releases")
    output_path = f"{path}/releases/{name}.zip"
    if os.path.exists(output_path):
        os.remove(output_path)
    with zipfile.ZipFile(output_path, 'w', compression=zipfile.ZIP_DEFLATED) as zipf:
        zipf.write("build/merged-binary.bin", arcname=f"{name}_full.bin")
        if os.path.exists("build/merged-binary-licence.bin"):
            zipf.write("build/merged-binary-licence.bin", arcname=f"{name}_full_licence.bin")
        zipf.write("build/xiaozhi.bin", arcname=f"{name}_ota.bin")
        zipf.write("build/xiaozhi.elf", arcname=f"{name}.elf")
    print(f"zip bin to {output_path} done")
    

def release_current(path = "./"):
    merge_bin()
    board_type = get_board_type()
    print("board type:", board_type)
    project_version = get_project_version()
    print("project version:", project_version)
    zip_bin(board_type, project_version, path)

def release(board_path):    
    if os.path.exists(f"{board_path}/boards.cfg"):
        if os.system(f"idf.py @{board_path}/boards.cfg build") != 0:
            print("build failed")
            sys.exit(1)
    else:
        if os.system(f"idf.py @{board_path}/build.cfg build") != 0:
            print("build failed")
            sys.exit(1)
    release_current(board_path)

if __name__ == "__main__":
    if len(sys.argv) > 1:
        board_path = sys.argv[1]
        if len(sys.argv) == 3 and sys.argv[2] == "no_clean":
            pass
        else:
            os.system("rm -rf build;rm sdkconfig;rm main/assets/lang_config.h")
        release(board_path)
    else:
        release_current()
