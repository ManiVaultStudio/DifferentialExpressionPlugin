#pragma once

#include <actions/DatasetPickerAction.h>
#include <actions/TriggerAction.h>
#include <util/Serializable.h>

#include <Dataset.h>
#include <LinkedData.h>
#include <PointData/PointData.h>
#include <Set.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <QDialog>

// 
// Utility functions
//

// This only checks the immedeate parent and is deliberately not recursive
// We might consider the latter in the future, but might need to cover edge cases
inline bool parentHasSameNumPoints(const mv::Dataset<mv::DatasetImpl> data, const mv::Dataset<Points>& other) {
    if (!data->isDerivedData())
        return false;

    const auto parent = data->getParent();
    if (parent->getDataType() != PointType)
        return false;

    const auto parentPoints = mv::Dataset<Points>(parent);
    return parentPoints->getNumPoints() == other->getNumPoints();
}

// Is the data derived and does it's full source data have same number of points as the other data
inline bool fullSourceHasSameNumPoints(const mv::Dataset<mv::DatasetImpl> data, const mv::Dataset<Points>& other) {
    if (!data->isDerivedData())
        return false;

    return data->getSourceDataset<Points>()->getFullDataset<Points>()->getNumPoints() == other->getNumPoints();
}

using LinkedDataCondition = std::function<bool(const mv::LinkedData& linkedData, const mv::Dataset<Points>& target)>;

/*  Returns a mapping (linked data) from source that fulfils a given condition based on target, e.g.
    auto checkMapping = [](const mv::LinkedData& linkedData, const mv::Dataset<Points>& target) -> bool {
        return linkedData.getTargetDataset() == target;
        };
    This function will return the first match of the condition
*/
std::pair<const mv::LinkedData*, unsigned int> getSelectionMapping(const mv::Dataset<Points>& source, const mv::Dataset<Points>& target, LinkedDataCondition checkMapping);

// Returns a mapping (linked data) from other whose target is current
std::pair<const mv::LinkedData*, unsigned int> getSelectionMappingOtherToCurrent(const mv::Dataset<Points>& other, const mv::Dataset<Points>& current);

// Check if the mapping is surjective, i.e. hits all elements in the target
bool checkSurjectiveMapping(const mv::LinkedData* linkedData, const std::uint32_t numPointsInTarget);

// returns whether there is a selection map from other to current or current to other (or respective parents)
// checks whether the mapping covers all elements in the target
bool checkSelectionMapping(const mv::Dataset<Points>& other, const mv::Dataset<Points>& current);

// 
// AdditionalSettingsDialog
//

/*  Dialog available via right-click on the widget
    Current additional settings:
        - Select another data set (which must have a surjective selection to the current data)
          which will be used as the source for highlighting indices
*/
class AdditionalSettingsDialog : public QDialog, public mv::util::Serializable
{
    Q_OBJECT
public:
    AdditionalSettingsDialog();

    void populateAndOpenDialog();

public: // Serialization

    void fromVariantMap(const QVariantMap& variantMap) override;
    QVariantMap toVariantMap() const override;


public: // Getter

    mv::gui::DatasetPickerAction& getSelectionMappingSourcePicker() { return _selectionMappingSourcePicker; }

    std::vector<uint32_t>& getSelection(const QString& selectionName) {
        if (selectionName == "A")
            return _selectionA;

        return _selectionB;
    }

    std::vector<uint32_t>& getSelectionA() { return _selectionA; }
    std::vector<uint32_t>& getSelectionB() { return _selectionB; }

public: // Setter

    void setCurrentData(const mv::Dataset<Points>& currentData) { _currentData = currentData; }

private:
    mv::gui::TriggerAction          _okButton;
    mv::gui::DatasetPickerAction    _selectionMappingSourcePicker;

    mv::Dataset<Points>             _currentData = {};

    std::vector<uint32_t>           _selectionA = {};
    std::vector<uint32_t>           _selectionB = {};
};
