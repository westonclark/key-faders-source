/*
  ==============================================================================

    This file contains the basic startup code for a JUCE application.

  ==============================================================================
*/

#include <JuceHeader.h>
#include "FaderEngine.h"
#include "GlobalKeyListener.h"
#include "TrayIconMac.h"
#include "RegistrationManager.h"
#include "RegistrationDialog.h"

//==============================================================================
// Simple registration component
class RegistrationComponent : public juce::Component
{
public:
    RegistrationComponent(std::function<bool(const juce::String&)> registrationFunc)
        : onRegister(registrationFunc)
    {
        serialNumberInput.setTextToShowWhenEmpty("Enter Serial Number", juce::Colours::grey);
        addAndMakeVisible(serialNumberInput);

        registerButton.setButtonText("Register");
        registerButton.onClick = [this] { attemptRegistration(); };
        addAndMakeVisible(registerButton);

        setSize(300, 100);
    }

private:
    void attemptRegistration()
    {
        if (onRegister(serialNumberInput.getText()))
        {
            // If registration is successful, close the window
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(1);  // 1 indicates success
        }
        else
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Invalid Serial Number",
                "Please enter a valid serial number.");
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10);
        serialNumberInput.setBounds(bounds.removeFromTop(30));
        bounds.removeFromTop(10);
        registerButton.setBounds(bounds.removeFromTop(30));
    }

    std::function<bool(const juce::String&)> onRegister;
    juce::TextEditor serialNumberInput;
    juce::TextButton registerButton;
};

//==============================================================================
// Main application class
class FaderKeysApplication : public juce::JUCEApplication
{
public:
    FaderKeysApplication()
    {
        // Initialize once in constructor
        appProperties = std::make_unique<juce::ApplicationProperties>();
        appProperties->setStorageParameters(getPropertyFileOptions());
        registrationManager = std::make_unique<RegistrationManager>(*appProperties);
    }

    //==============================================================================
    const juce::String getApplicationName() override      { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override   { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override            { return true; }

    //==============================================================================
    void initialise(const juce::String&) override
    {
        // Create tray icon first, but with engine disabled
        TrayIconMac::createStatusBarIcon(nullptr, false);

        if (!registrationManager->isRegistered())
        {
            RegistrationDialog::show(
                [this](const juce::String& serial, std::function<void(bool)> callback)
                {
                    registrationManager->registerSerialNumberAsync(serial,
                        [this, callback](bool success)
                        {
                            if (success)
                            {
                                showRegistrationSuccessMessage();
                                // Don't call finishStartup() here
                            }
                            if (callback)
                                callback(success);
                        });
                });
            return;
        }

        finishStartup();  // Only called on subsequent launches when already registered
    }

    void shutdown() override
    {
        // Save sensitivity settings before cleanup
        if (faderEngine != nullptr)
        {
            auto* settings = appProperties->getUserSettings();
            settings->setValue("nudgeSensitivity", (int)faderEngine->getNudgeSensitivity());
            settings->saveIfNeeded();
        }

        // Ensure proper cleanup order
        // Remove the tray icon
        TrayIconMac::removeStatusBarIcon();
        // Stop the global key listener
        stopGlobalKeyListener();
        // Reset the FaderEngine
        faderEngine.reset();

        // Clean up the dialog if it's still around
        if (activeDialog != nullptr)
        {
            activeDialog->exitModalState(0);
            activeDialog = nullptr;
        }
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
    }

    //==============================================================================
    juce::ApplicationProperties& getAppProperties()
    {
        // Return reference to existing instance
        jassert(appProperties != nullptr);
        return *appProperties;
    }

    bool isRegistered() const
    {
        auto* settings = appProperties->getUserSettings();
        bool registered = settings->getBoolValue("isRegistered", false);
        return registered;
    }

    bool registerSerialNumber(const juce::String& serialNumber)
    {
        // Validate the serial number via HTTP
        const bool isValid = validateSerialNumber(serialNumber);
        if (isValid)
        {
            auto* settings = appProperties->getUserSettings();
            settings->setValue("isRegistered", true);
            settings->setValue("serialNumber", serialNumber);
            settings->saveIfNeeded();

            // Show success message and quit
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                "Registration Successful",
                "Please relaunch Fader Keys to complete setup.",
                "OK",
                nullptr,
                juce::ModalCallbackFunction::create([](int) {
                    juce::Timer::callAfterDelay(500, []() {
                        juce::JUCEApplication::getInstance()->systemRequestedQuit();
                    });
                }));
        }
        return isValid;
    }

private:
    // Setup the property file location
    juce::PropertiesFile::Options getPropertyFileOptions()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = getApplicationName();
        options.filenameSuffix     = ".settings";
        options.folderName         = "FaderKeys";
        options.osxLibrarySubFolder = "Application Support";

        return options;
    }

    bool validateSerialNumber(const juce::String& serialNumber)
    {
        // Create URL object for the authentication endpoint
        juce::URL url("https://www.faderkeys.com/api/auth/serial");

        // Create the JSON request body
        juce::var jsonBody = juce::var(new juce::DynamicObject());
        jsonBody.getDynamicObject()->setProperty("serialNumber", serialNumber);

        const juce::String jsonString = juce::JSON::toString(jsonBody);

        // Set up request headers
        juce::URL::InputStreamOptions opts(juce::URL::ParameterHandling::inPostData);
        auto newOpts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                          .withExtraHeaders("Content-Type: application/json")
                          .withConnectionTimeoutMs(5000);

        // Attach the JSON payload as the POST data
        url = url.withPOSTData(jsonString);

        // Store the return value of createInputStream
        auto inputStream = url.createInputStream(newOpts);
        if (inputStream != nullptr)
        {
            const juce::String response = inputStream->readEntireStreamAsString();
            auto parsed = juce::JSON::parse(response);

            if (auto* webStream = dynamic_cast<juce::WebInputStream*>(inputStream.get()))
            {
                const int statusCode = webStream->getStatusCode();
                // If 200, success; if 401 (or anything else), fail
                return statusCode == 200;
            }
        }
        return false;
    }

    void finishStartup()
    {
        auto* settings = getAppProperties().getUserSettings();
        auto lastSensitivity = static_cast<FaderEngine::NudgeSensitivity>(
            settings->getIntValue("nudgeSensitivity",
                                  static_cast<int>(FaderEngine::NudgeSensitivity::Medium)));

        // Create FaderEngine first
        faderEngine = std::make_unique<FaderEngine>();
        faderEngine->setNudgeSensitivity(lastSensitivity);

        // Start key listener before creating tray icon
        startGlobalKeyListener(faderEngine.get());


        // Remove the old tray icon and create new one with engine enabled
        TrayIconMac::removeStatusBarIcon();
        TrayIconMac::createStatusBarIcon(faderEngine.get(), true);
        TrayIconMac::updateSensitivityMenu(lastSensitivity);


    }

    void showRegistrationSuccessMessage()
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon,
            "Registration Successful",
            "Please relaunch Fader Keys to complete setup.",
            "OK",
            nullptr,
            juce::ModalCallbackFunction::create([](int) {
                juce::Timer::callAfterDelay(500, []() {
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
                });
            }));
    }

    std::unique_ptr<FaderEngine>                faderEngine;
    std::unique_ptr<juce::ApplicationProperties> appProperties;
    std::unique_ptr<RegistrationManager> registrationManager;
    juce::DialogWindow* activeDialog = nullptr;
};

//==============================================================================
START_JUCE_APPLICATION(FaderKeysApplication)
