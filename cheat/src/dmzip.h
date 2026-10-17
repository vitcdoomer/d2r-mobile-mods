/* dmzip.h — 极简 ZIP（仅 store，不压缩）写入/解出，纯 C、不依赖任何系统头文件。
 *
 * 用途：「存档一键搬家」——导出时把 Documents/save 目录树打成 zip；导入时解回。
 * 特点：
 *   - 只写/只读 method=0（stored）条目 —— 即本模块自己写出的包；
 *   - 名称一律 UTF-8（general purpose flag bit 11）—— 跨设备/电脑通用，PC 上可直接打开；
 *   - 解压端做路径安全校验（拒绝绝对路径 / 反斜杠 / ".." 穿越 / 空组件）；
 *   - 只用静态内存（单实例、串行使用），不依赖 malloc；
 *   - 目录创建走平台钩子 dmzip_platform_mkdir（iOS 与 Windows 自测各自提供）。
 *
 * 导出端（目录遍历由调用方完成，逐文件调用 writer_add）：
 *   void *w = dmzip_writer_open("out.zip");
 *   dmzip_writer_add(w, "0000000000000001/0/joe.d2s", "<绝对路径>");
 *   dmzip_writer_close(w);
 * 导入端：
 *   int rc = dmzip_unzip_file("in.zip", "<目标目录>");   // 0 = 成功
 */
#ifndef DMZIP_H
#define DMZIP_H

enum {
    DZ_OK = 0,
    DZ_E_OPEN = -1,     /* 打不开文件 */
    DZ_E_READ = -2,     /* 读失败 */
    DZ_E_WRITE = -3,    /* 写失败 */
    DZ_E_FORMAT = -4,   /* 不是本模块的 zip / 内容损坏（CRC 不符） */
    DZ_E_PATH = -5,     /* 路径不安全（绝对路径 / 反斜杠 / ".."）或过长 */
    DZ_E_MANY = -6,     /* 条目过多（> 512） */
    DZ_E_METHOD = -7,   /* 压缩方式不支持（非 store / 带数据描述符） */
    DZ_E_STATE = -8,    /* 状态错误（重复 open 等） */
    DZ_E_TOOBIG = -9    /* 单文件 ≥ 4GB（本格式放不下） */
};

void *dmzip_writer_open(const char *zip_path);
int   dmzip_writer_add(void *w, const char *rel_name, const char *src_path);
int   dmzip_writer_close(void *w);
int   dmzip_unzip_file(const char *zip_path, const char *dst_root);

#endif
