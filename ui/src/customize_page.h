#pragma once

#include <QWidget>

#include <functional>
#include <vector>

namespace stud::ui {

// Settings' Customize tab: a card per generated customization, grouped, showing what
// each has now. Redraws whenever it is shown, as Roblox may have installed or a
// crash set files aside since.
class CustomizePage : public QWidget {
public:
    // `import` brings in another bootstrapper's customizations.
    CustomizePage(std::function<void()> import, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refresh();

    std::vector<std::function<void()>> refreshers_;
};

}  // namespace stud::ui
