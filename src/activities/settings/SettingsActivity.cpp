#include "SettingsActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "CategorySettingsActivity.h"
#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "MappedInputManager.h"
#include "fontIds.h"

const char* SettingsActivity::categoryNames[categoryCount] = {"Écran", "Lecture", "Contrôles", "Système"};

namespace {
constexpr int displaySettingsCount = 5;
const SettingInfo displaySettings[displaySettingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum("Écran de veille", &CrossPointSettings::sleepScreen, {"Sombre", "Clair", "Personalisé", "Couverture", "Aucun"}),
    SettingInfo::Enum("Mode d'écran de veille", &CrossPointSettings::sleepScreenCoverMode, {"Étiré", "Recadré"}),
    SettingInfo::Enum("Barre d'état", &CrossPointSettings::statusBar, {"Aucune", "Sans progrès", "Complète"}),
    SettingInfo::Enum("Cacher % de batterie", &CrossPointSettings::hideBatteryPercentage, {"Jamais", "En lecture", "Toujours"}),
    SettingInfo::Enum("Fréquence de refraîchement", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"})};

constexpr int readerSettingsCount = 10;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum("Police", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic", "Personalisée"}),
    SettingInfo::Action("Définir la police personalisée"),
    SettingInfo::Enum("Taille de police", &CrossPointSettings::fontSize, {"Petit", "Moyenne", "Grande", "Très grande"}),
    SettingInfo::Enum("Espacement des lignes", &CrossPointSettings::lineSpacing, {"Fin", "Moyen", "Large"}),
    SettingInfo::Value("Marge d'écran", &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum("Alignement", &CrossPointSettings::paragraphAlignment,
                      {"Justifié", "À droite", "Centré", "À gauche"}),
    SettingInfo::Toggle("Césure", &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum("Orientation", &CrossPointSettings::orientation,
                      {"Portrait", "90° vers droite", "Inversée", "90° vers gauche"}),
    SettingInfo::Toggle("Espacement après paragraphes", &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Anticrénelage", &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    SettingInfo::Enum("Dispo. des boutons", &CrossPointSettings::frontButtonLayout,
                      {"Rtr, Cnfrm, Gch, Drt", "Gch, Drt, Rtr, Cnfrm", "Gch, Rtr, Cnfrm, Drt"}),
    SettingInfo::Enum("Dispo des boutons (droite)", &CrossPointSettings::sideButtonLayout,
                      {"Prcdnt, Prchne", "Prchne, Prcdnt"}),
    SettingInfo::Toggle("Appui long pour sauter le chapitre", &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum("Appui court du bouton alim.", &CrossPointSettings::shortPwrBtn, {"Ignoré", "M. en veille", "Prchne page"})};

constexpr int systemSettingsCount = 5;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum("Temps avant veille", &CrossPointSettings::sleepTimeout,
                      {"1 min.", "5 min.", "10 min.", "15 min.", "30 min."}),
    SettingInfo::Action("Synchro. KOReader"), SettingInfo::Action("Paramètres Calibre"), SettingInfo::Action("Vider cache"),
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
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Handle category selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    enterCategory(selectedCategoryIndex);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    // Reload fonts to make sure the newly selected font settings are loaded
    EpdFontLoader::loadFontsFromSd(renderer);
    onGoHome();
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Move selection up (with wrap-around)
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Move selection down (with wrap around)
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    updateRequired = true;
  }
}

void SettingsActivity::enterCategory(int categoryIndex) {
  if (categoryIndex < 0 || categoryIndex >= categoryCount) {
    return;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();

  const SettingInfo* settingsList = nullptr;
  int settingsCount = 0;

  switch (categoryIndex) {
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

  enterNewActivity(new CategorySettingsActivity(renderer, mappedInput, categoryNames[categoryIndex], settingsList,
                                                settingsCount, [this] {
                                                  exitActivity();
                                                  updateRequired = true;
                                                }));
  xSemaphoreGive(renderingMutex);
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

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Paramètres", true, EpdFontFamily::BOLD);

  // Draw selection
  renderer.fillRect(0, 60 + selectedCategoryIndex * 30 - 2, pageWidth - 1, 30);

  // Draw all categories
  for (int i = 0; i < categoryCount; i++) {
    const int categoryY = 60 + i * 30;  // 30 pixels between categories

    // Draw category name
    renderer.drawText(UI_10_FONT_ID, 20, categoryY, categoryNames[i], i != selectedCategoryIndex);
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Retour", "Select.", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
