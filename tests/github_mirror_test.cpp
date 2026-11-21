#include "github_mirror.h"

#include "check.h"

using stud::ui::github::jsdelivr_copy;

static std::string copy_of(const char* url) { return jsdelivr_copy(QUrl(QString::fromLatin1(url))).toString().toStdString(); }

int main() {
    check_eq(copy_of("https://raw.githubusercontent.com/bloxstraplabs/bloxstrap/HEAD/Bloxstrap/Resources/Mods/Cursor/From2013/ArrowCursor.png"),
             "https://cdn.jsdelivr.net/gh/bloxstraplabs/bloxstrap@HEAD/Bloxstrap/Resources/Mods/Cursor/From2013/ArrowCursor.png",
             "a raw GitHub file maps to jsDelivr at the same ref and path");
    check_eq(copy_of("https://raw.githubusercontent.com/Froststrap/config/main/Data.json"),
             "https://cdn.jsdelivr.net/gh/Froststrap/config@main/Data.json", "a file at the repository's root");
    check_eq(copy_of("https://raw.githubusercontent.com/owner/repository/HEAD"), "", "no file named: no copy");
    check_eq(copy_of("https://api.github.com/repos/CatPieLeaf/Stud/releases/latest"), "", "the API has no copy");
    check_eq(copy_of("https://cdn.froststrap.xyz/Download/Snowy.zip"), "", "other hosts have no copy");
    return 0;
}
