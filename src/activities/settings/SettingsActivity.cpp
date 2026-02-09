#include "SettingsActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const char* SettingsActivity::categoryNames[categoryCount] = {"Écran", "Lecture", "Contrôles", "Système"};

namespace {
constexpr int changeTabsMs = 700;
constexpr int displaySettingsCount = 8;
const SettingInfo displaySettings[displaySettingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum("Écran de veille", &CrossPointSettings::sleepScreen,
                      {"Sombre", "Clair", "Personnalisé", "Couverture", "Aucun", "Couv. + Perso."}),
    SettingInfo::Enum("Mode d'écran de veille", &CrossPointSettings::sleepScreenCoverMode, {"Étiré", "Recadré"}),
    SettingInfo::Enum("Filtre de l'écran de veille", &CrossPointSettings::sleepScreenCoverFilter,
                      {"Aucun", "Contraste", "Inversé"}),
    SettingInfo::Enum(
        "Barre d'état", &CrossPointSettings::statusBar,
        {"Aucune", "Sans progrès", "Pleine avec %", "Pleine avec progrès", "Barre du livre", "Barre de progrès"}),
    SettingInfo::Enum("Cacher % de batterie", &CrossPointSettings::hideBatteryPercentage, {"Jamais", "En lecture", "Toujours"}),
    SettingInfo::Enum("Fréquence de refraîchement", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
    SettingInfo::Enum("Thème", &CrossPointSettings::uiTheme, {"Classique", "Lyra"}),
    SettingInfo::Toggle("Correction du soleil", &CrossPointSettings::fadingFix)};

constexpr int readerSettingsCount = 11;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum("Police", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic", "Personalisée"}),
    SettingInfo::Action("Définir la police personalisée"),
    SettingInfo::Enum("Taille de police", &CrossPointSettings::fontSize, {"Petit", "Moyenne", "Grande", "Très grande"}),
    SettingInfo::Enum("Espacement des lignes", &CrossPointSettings::lineSpacing, {"Fin", "Moyen", "Large"}),
    SettingInfo::Value("Marge d'écran", &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum("Alignement", &CrossPointSettings::paragraphAlignment,
                      {"Justifié", "À droite", "Centré", "À gauche"}),
    SettingInfo::Toggle("Style incluse", &CrossPointSettings::embeddedStyle),
    SettingInfo::Toggle("Césure", &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum("Orientation", &CrossPointSettings::orientation,
                      {"Portrait", "90° vers droite", "Inversée", "90° vers gauche"}),
    SettingInfo::Toggle("Espacement après paragraphes", &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Anticrénelage", &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    SettingInfo::Action("Disposition des boutons"),
    SettingInfo::Enum("Dispo. des boutons (droite)", &CrossPointSettings::sideButtonLayout,
                      {"Prcdnt, Prchne", "Prchne, Prcdnt"}),
    SettingInfo::Toggle("Appui long pour sauter le chapitre", &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum("Appui court du bouton alim.", &CrossPointSettings::shortPwrBtn, {"Ignoré", "M. en veille", "Prchne page"})};

constexpr int systemSettingsCount = 5;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum("Temps avant veille", &CrossPointSettings::sleepTimeout,
                      {"1 min.", "5 min.", "10 min.", "15 min.", "30 min."}),
    SettingInfo::Action("Synchro. KOReader"), SettingInfo::Action("Navigateur OPDS"), SettingInfo::Action("Vider cache"),
    SettingInfo::Action("Vérifier les mises à jour")};
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  settingsList = displaySettings;
  settingsCount = displaySettingsCount;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      updateRequired = true;
    } else {
      toggleCurrentSetting();
      updateRequired = true;
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    // Reload fonts to make sure the newly selected font settings are loaded
    EpdFontLoader::loadFontsFromSd(renderer);
    onGoHome();
    return;
  }

  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool changeTab = mappedInput.getHeldTime() > changeTabsMs;

  // Handle navigation
  if (upReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    updateRequired = true;
  } else if (downReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    updateRequired = true;
  } else if (upReleased || leftReleased) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount);
    updateRequired = true;
  } else if (rightReleased || downReleased) {
    selectedSettingIndex = (selectedSettingIndex < settingsCount) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:  // Display
        settingsList = displaySettings;
        settingsCount = displaySettingsCount;
        break;
      case 1:  // Reader
        settingsList = readerSettings;
        settingsCount = readerSettingsCount;
        break;
      case 2:  // Controls
        settingsList = controlsSettings;
        settingsCount = controlsSettingsCount;
        break;
      case 3:  // System
        settingsList = systemSettings;
        settingsCount = systemSettingsCount;
        break;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = settingsList[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (strcmp(setting.name, "Disposition des boutons") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Syncro. KOReader") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Navigateur OPDS") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Vider cache") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Vérifier les mises à jour") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Définir la police personalisée") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new FontSelectionActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    }
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void SettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Paramètres");

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({categoryNames[i], selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1, [this](int index) { return std::string(settingsList[index].name); },
      nullptr, nullptr,
      [this](int i) {
        const auto& setting = settingsList[i];
        std::string valueText = "";
        if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
          const bool value = SETTINGS.*(settingsList[i].valuePtr);
          valueText = value ? "ON" : "OFF";
        } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
          valueText = settingsList[i].enumValues[value];
        } else if (settingsList[i].type == SettingType::VALUE && settingsList[i].valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(settingsList[i].valuePtr));
        }
        return valueText;
      });

  // Draw version text
  renderer.drawText(SMALL_FONT_ID,
                    pageWidth - metrics.versionTextRightX - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    metrics.versionTextY, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Retour", "Select.", "Haut", "Bas");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
