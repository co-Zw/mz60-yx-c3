import sys
import os
import shutil
import re

# 切换到项目根目录
os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def updated_board(file, old_board, new_board):
    with open(file, "r", encoding="utf-8") as f:
        content = f.read()

    # 使用正则表达式进行匹配和替换
    updated_content = re.sub(old_board, new_board, content)

    # 将修改后的内容写回到文件
    with open(file, "w", encoding="utf-8") as f:
        f.write(updated_content)

if __name__ == "__main__":
    if len(sys.argv) == 3:
        template = sys.argv[1]
        project = sys.argv[2]
        if not os.path.exists(template):
            print(f"{template} not exists")
            sys.exit(1)
        
        if os.path.exists(project):
            # shutil.rmtree(project)
            print(f"{project} is exists")
            sys.exit(1)

        shutil.copytree(template, project)
        
        if os.path.exists(f"{project}/releases"):
            shutil.rmtree(f"{project}/releases")
            
        if os.path.exists(f"{project}/firmware"):
            shutil.rmtree(f"{project}/firmware")
        
        if os.path.exists(f"{project}/boards.cfg"):
            updated_board(f"{project}/boards.cfg", template, project)
            updated_board(f"{project}/boards.defaults", template, project)
        
        if os.path.exists(f"{project}/build.cfg"):
            updated_board(f"{project}/build.cfg", template, project)
            updated_board(f"{project}/sdkconfig.defaults.board", template, project)
        
        print(f"{project} generate done")
    else:
        print("please use: python firmware/project_gen.py template project")
