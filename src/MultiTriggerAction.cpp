#include "MultiTriggerAction.h"

#include <QMenu>

MultiTriggerAction::MultiTriggerAction(QObject* object, const QString& title, const QString& trigger_title):
    GroupAction(object,title,true),
    _triggerAction()
{
    int id = 1;
    for (auto &action : _triggerAction)
    {
        action.reset(new TriggerAction(this, trigger_title.arg(id++)));
        action->setCheckable(false);
        action->setChecked(false);
    }
}

MultiTriggerAction::~MultiTriggerAction()
{
}

QMenu* MultiTriggerAction::getContextMenu(QWidget* parent)
{
    auto menu = new QMenu(text(), parent);

    for (auto action : _triggerAction)
        menu->addAction(action.get());
    

    return menu;
}

TriggerAction* MultiTriggerAction::getTriggerAction(uint8_t id)
{
    if (id < _triggerAction.size())
        return _triggerAction[id].get();
    return nullptr;
}


void MultiTriggerAction::fromVariantMap(const QVariantMap& variantMap) 
{
    GroupAction::fromVariantMap(variantMap);

    for (auto& action : _triggerAction)
    {
        if (variantMap.contains(action->getSerializationName()))
            action->fromParentVariantMap(variantMap);

    }
}

QVariantMap MultiTriggerAction::toVariantMap() const
{
    QVariantMap variantMap = GroupAction::toVariantMap();
    for (auto& action : _triggerAction)
        action->insertIntoVariantMap(variantMap);

    return variantMap;
}