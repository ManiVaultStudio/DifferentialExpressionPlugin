#pragma once

#include <ViewPlugin.h>

#include "LoadedDatasetsAction.h"
#include "MultiTriggerAction.h"

#include <Dataset.h>
#include <widgets/DropWidget.h>

#include <PointData/PointData.h>
#include <ClusterData/ClusterData.h>


#include <QTableWidget>
#include "TableSortFilterProxyModel.h"
#include "TableModel.h"
#include "TableView.h"
#include "ButtonProgressBar.h"

/** All plugin related classes are in the ManiVault plugin namespace */
using namespace mv::plugin;

/** Drop widget used in this plugin is located in the ManiVault gui namespace */
using namespace mv::gui;

/** Dataset reference used in this plugin is located in the ManiVault util namespace */
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

    /**
     * Invoked when a data event occurs
     * @param dataEvent Data event which occurred
     */
    void onDataEvent(mv::DatasetEvent* dataEvent);


    void setPositionDataset(mv::Dataset<Points> newPoints);
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
    DropWidget*             _dropWidget;                /** Widget for drag and drop behavior */
    mv::Dataset<Points>     _points;                    /** Points smart pointer */
    QString                 _currentDatasetName;        /** Name of the current dataset */
    QLabel*                 _currentDatasetNameLabel;   /** Label that show the current dataset name */

    MultiTriggerAction      _selectionTriggerActions;
    QLabel                  _selectedCellsLabel[MultiTriggerAction::Size];  
   
    QStringList             _geneList;

    LoadedDatasetsAction    _loadedDatasetsAction;

    TriggerAction                       _updateStatisticsAction;
    StringAction                        _filterOnIdAction;
    StringAction                         _selectedIdAction;
    QSharedPointer<TableModel>     _tableItemModel;
    QPointer<TableSortFilterProxyModel>      _sortFilterProxyModel;
    TableView*                          _tableView;
    QPointer<ButtonProgressBar>         _buttonProgressBar;
    TriggerAction                       _copyToClipboardAction;
    TriggerAction                       _saveToCsvAction;

    QVector<WidgetAction*>              _serializedActions;
    QByteArray                          _headerState;


    std::vector<QTableWidgetItem*> _geneTableItems;
    std::vector<QTableWidgetItem*> _diffTableItems;

    std::vector<float> minValues;
    std::vector<float> rescaleValues;

    int selOpt = 0;
    std::vector<uint32_t> selectionA;
    std::vector<uint32_t> selectionB;

    // TEMP: toggle for normalization within the loaded dataset
    ToggleAction _normAction; // min max normalization
    bool _norm = false;
};

/**
 * Example view plugin factory class
 *
 * Note: Factory does not need to be altered (merely responsible for generating new plugins when requested)
 */
class DifferentialExpressionPluginFactory : public ViewPluginFactory
{
    Q_INTERFACES(mv::plugin::ViewPluginFactory mv::plugin::PluginFactory)
    Q_OBJECT
    Q_PLUGIN_METADATA(IID   "nl.BioVault.DifferentialExpressionPlugin"
                      FILE  "DifferentialExpressionPlugin.json")

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
