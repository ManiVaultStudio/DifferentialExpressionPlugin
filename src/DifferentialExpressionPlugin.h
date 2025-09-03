#pragma once

#include <Dataset.h>
#include <ViewPlugin.h>
#include <PointData/DimensionPickerAction.h>
#include <PointData/PointData.h>
#include <widgets/DropWidget.h>

#include "ButtonProgressBar.h"
#include "LoadedDatasetsAction.h"
#include "MultiTriggerAction.h"
#include "TableModel.h"
#include "TableSortFilterProxyModel.h"
#include "TableView.h"

#include <array>

#include <QTableWidget>

using namespace mv::plugin;
using namespace mv::gui;
using namespace mv::util;

class QLabel;

class DifferentialExpressionPlugin : public ViewPlugin
{
    Q_OBJECT

public:

    /**
     * Constructor
     * @param factory Pointer to the plugin factory
     */
    DifferentialExpressionPlugin(const PluginFactory* factory);

    /** Destructor */
    ~DifferentialExpressionPlugin() override = default;
    
    /** This function is called by the core after the view plugin has been created */
    void init() override;

    void setPositionDataset(const mv::Dataset<Points>& newPoints);
    /** Invoked when the position points dataset changes */
    void positionDatasetChanged();

public: // Miscellaneous
    /** Get smart pointer to points dataset for point position */
    mv::Dataset<Points>& getPositionDataset() { return _points; }

public: // Serialization

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
 

protected slots:
    void writeToCSV() const;
    void computeDE();
    
    void tableView_clicked(const QModelIndex& index);
    void tableView_selectionChanged(const QItemSelection& selected, const QItemSelection& deselected);


protected:
    using QLabelArray2 = std::array<QLabel, MultiTriggerAction::Size>;

    DropWidget*                             _dropWidget;                /** Widget for drag and drop behavior */
    mv::Dataset<Points>                     _points;                    /** Points smart pointer */
    QLabel*                                 _currentDatasetNameLabel;   /** Label that show the current dataset name */
   
    MultiTriggerAction                      _selectionTriggerActions;
    LoadedDatasetsAction                    _loadedDatasetsAction;
    TriggerAction                           _updateStatisticsAction;
    StringAction                            _filterOnIdAction;
    StringAction                            _selectedIdAction;
    TriggerAction                           _copyToClipboardAction;
    TriggerAction                           _saveToCsvAction;
    DimensionPickerAction                   _currentSelectedDimension;

    QLabelArray2                            _selectedCellsLabel;
    int                                     _totalTableColumns;
    QSharedPointer<TableModel>              _tableItemModel;
    QPointer<TableSortFilterProxyModel>     _sortFilterProxyModel;
    TableView*                              _tableView;
    QPointer<ButtonProgressBar>             _buttonProgressBar;

    QVector<WidgetAction*>                  _serializedActions;
    QByteArray                              _headerState;

    std::vector<QTableWidgetItem*>          _geneTableItems;
    std::vector<QTableWidgetItem*>          _diffTableItems;

    std::vector<float>                      _minValues;
    std::vector<float>                      _rescaleValues;

    std::vector<uint32_t>                   _selectionA;
    std::vector<uint32_t>                   _selectionB;

    // TEMP: toggle for normalization within the loaded dataset
    ToggleAction                            _normAction; // min max normalization
    bool                                    _norm = false;
};


// =============================================================================
// Factory
// =============================================================================
class DifferentialExpressionPluginFactory : public ViewPluginFactory
{
    Q_INTERFACES(mv::plugin::ViewPluginFactory mv::plugin::PluginFactory)
    Q_OBJECT
    Q_PLUGIN_METADATA(IID   "nl.BioVault.DifferentialExpressionPlugin"
                      FILE  "PluginInfo.json")

public:

    /** Default constructor */
    DifferentialExpressionPluginFactory();

    /** Destructor */
    ~DifferentialExpressionPluginFactory() override {}
    
    /** Creates an instance of the example view plugin */
    ViewPlugin* produce() override;

    /** Returns the data types that are supported by the example view plugin */
    mv::DataTypes supportedDataTypes() const override;

    /**
     * Get plugin trigger actions given \p datasets
     * @param datasets Vector of input datasets
     * @return Vector of plugin trigger actions
     */
    PluginTriggerActions getPluginTriggerActions(const mv::Datasets& datasets) const override;
};
