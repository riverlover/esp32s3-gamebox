# pce-go —— PC Engine / TurboGrafx-16 模拟器核心
#
# 源码取自 retro-go（GPL v2，见 COPYING / CREDITS），未做修改。
# 与框架的粘合全在 shim/rg_system.h 这一个垫片文件里。
#
# 上游追到 HuExpress -> Hu-Go! -> BERO 的 PCE 模拟器，CREDITS 里有完整链条。

idf_component_register(
    SRC_DIRS         "."
    INCLUDE_DIRS     "."
    PRIV_INCLUDE_DIRS "shim"
    REQUIRES         esp_common esp_rom
)

# RETRO_GO 只能是 PRIVATE，和 nofrendo 那边的 PUBLIC 正相反：
# nofrendo 需要 PUBLIC 是因为 main 会连带 include nes/utils.h，两边对
# IRAM_ATTR 的取值必须一致；pce-go 根本没用 IRAM_ATTR，而它的 `#ifdef
# RETRO_GO` 分支只影响日志宏和 crc32_le 宏。设成 PRIVATE 之后 main 编
# pce-go.h 时走 `#else`，就不需要把这里的 rg_system.h 暴露出去。
target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO)

# -O3 是模拟器性能的关键。那几个 -Wno- 抄自上游自己的 CMakeLists，
# 是它本来就有的老式写法，不是我们引入的问题。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-sequence-point
    -Wno-unused
    -Wno-format
    -Wno-error=format
)
