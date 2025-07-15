#include <iostream>
#include <fstream>
#include <filesystem> // 确保包含了filesystem头文件
#include "VeritasSync/StateManager.h"

// create_dummy_files 函数保持不变
void create_dummy_files(const std::string& dir) {
  std::filesystem::path root(dir);
  std::filesystem::path docs_dir = root / "docs";
  if (!std::filesystem::exists(docs_dir)) {
    std::filesystem::create_directory(docs_dir);
  }
  std::ofstream file1(root / "hello.txt");
  file1 << "This is the first file." << std::endl;
  file1.close();
  std::ofstream file2(docs_dir / "report.txt");
  file2 << "This is a report." << std::endl;
  file2.close();
}

int main(int argc, char* argv[]) {
  std::cout << "--- Veritas Sync StateManager Test ---" << std::endl;

  std::string sync_folder = "SyncFolder";

  if (std::filesystem::exists(sync_folder)) {
    std::filesystem::remove_all(sync_folder);
  }
  create_dummy_files(sync_folder);
  std::cout << "[TestSetup] Dummy files and folder have been created/reset." << std::endl;

  // 创建StateManager实例
  VeritasSync::StateManager state_manager(sync_folder);

  // 执行扫描
  state_manager.scan_directory();

  // 打印扫描结果到控制台
  state_manager.print_current_state();

  // 获取并打印将要发送给其他节点的JSON消息
  std::string json_message = state_manager.get_state_as_json_string();
  std::cout << "\n--- Generated JSON Message ---" << std::endl;
  std::cout << json_message << std::endl;
  std::cout << "----------------------------" << std::endl;

  return 0;
}