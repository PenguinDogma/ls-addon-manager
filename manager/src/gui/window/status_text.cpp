#include "status_text.h"
#include "../../../sdk/include/eam/version.h"

namespace eam {
namespace window {

std::string StatusCounts(int total, int on) {
    return std::string(EAM_PRODUCT_SHORT " " EAM_VERSION_STRING "   |   ") + std::to_string(total) + (total == 1 ? " 个插件，" : " 个插件，") +
           std::to_string(on) + " 个已启用";
}

std::string WithUpdate(std::string line, const std::string& latest) {
    if (!latest.empty()) line += "   |   有可用更新：" + latest;
    return line;
}

std::string WithLiveStatus(std::string counts, const std::string& who, const std::string& text) {
    if (!text.empty()) counts += "   |   " + who + "：" + text;
    return counts;
}

} // namespace window
} // namespace eam
