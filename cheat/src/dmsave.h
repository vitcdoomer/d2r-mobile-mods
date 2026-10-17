/* dmsave.h — 存档一键搬家（导出/导入）对外接口。
 * 实现见 dmsave.c。UI（dmcheat_ui.c 的「存档」区按钮）在主线程调用。 */
#pragma once

void dmsave_export(void);   /* 主线程：打包 Documents/save → 系统「存储到」面板（可存 iCloud Drive） */
void dmsave_import(void);   /* 主线程：系统文件面板选存档包 → 自动备份旧档后覆盖导入（需重启 App 生效） */
