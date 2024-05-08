#pragma once
#include "actions/GroupAction.h"
#include "actions/TriggerAction.h"
#include <array>

using namespace mv;
using namespace mv::gui;
using namespace mv::util;

class QMenu;

namespace mv {
    class CoreInterface;
}

class MultiTriggerAction: public GroupAction
{

    
public:
    enum { Size = 2 };
    /**
     * Constructor
     * @param parent Pointer to parent object
     */

    MultiTriggerAction(QObject* object, const QString& title, const QString& trigger_title);
    ~MultiTriggerAction();

    /**
     * Get the context menu for the action
     * @param parent Parent widget
     * @return Context menu
     */
    QMenu* getContextMenu(QWidget* parent = nullptr) override;

    TriggerAction* getTriggerAction(uint8_t id);

    
public:// Serialization

    /**
     * Load plugin from variant map
     * @param Variant map representation of the plugin
     */
    void fromVariantMap(const QVariantMap& variantMap) override;

    /**
     * Save plugin to variant map
     * @return Variant map representation of the plugin
     */
    QVariantMap toVariantMap() const override;


private:
    std::array<QSharedPointer<TriggerAction>,Size> _triggerAction;
};