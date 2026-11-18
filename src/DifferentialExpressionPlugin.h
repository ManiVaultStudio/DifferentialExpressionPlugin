#pragma once

#include <Dataset.h>
#include <ViewPlugin.h>
#include <PointData/DimensionPickerAction.h>
#include <PointData/PointData.h>
#include <widgets/DropWidget.h>

#include "AdditionalSettings.h"
#include "ButtonProgressBar.h"
#include "LoadedDatasetsAction.h"
#include "MultiTriggerAction.h"
#include "TableModel.h"
#include "TableSortFilterProxyModel.h"
#include "TableView.h"

#include <array>
#include <functional>

#include <QTableWidget>

using namespace mv::plugin;
using namespace mv::gui;
using namespace mv::util;

class QLabel;

struct DifferentialExpressionResult;

/**
 * @brief View plugin for comparing expression across two point selections.
 *
 * The plugin owns its actions and table model, while its dataset handle keeps
 * the active point dataset available for reads. Differential-expression work
 * runs in a workflow worker stage; a following GUI-thread stage publishes the
 * result table while the plugin remains alive.
 *
 * @author Thomas Kroes
 */
class DifferentialExpressionPlugin : public ViewPlugin
{
    Q_OBJECT

public:

    /**
     * @brief Constructs the view plugin and its actions.
     * @param factory Factory that created this plugin instance.
     */
    DifferentialExpressionPlugin(const PluginFactory* factory);

    /** Destructor */
    ~DifferentialExpressionPlugin() override = default;
    
    /** This function is called by the core after the view plugin has been created */
    void init() override;

    /**
     * @brief Sets the point dataset used for selections and expression reads.
     * @param newPoints Point dataset to make active for this view.
     */
    void setPositionDataset(const mv::Dataset<Points>& newPoints);

    /** Invoked when the position points dataset changes */
    void positionDatasetChanged();

public: // Miscellaneous
    /**
     * @brief Gets the stored handle to the active points dataset.
     * @return Mutable reference to this view's dataset handle, valid while the view exists.
     */
    mv::Dataset<Points>& getPositionDataset() { return _points; }

public: // Serialization

    /**
     * @brief Restores serialized view and action state.
     * @param variantMap Serialized representation of this plugin instance.
     */
    void fromVariantMap(const QVariantMap& variantMap) override;

    /**
     * @brief Serializes the view and action state.
     * @return Variant map representation of this plugin instance.
     */
    QVariantMap toVariantMap() const override;
 

protected slots:

    /**
     * @brief Exports the current result table as a CSV file.
     *
     * Opens a save dialog and writes the model contents when a path is chosen.
     * The operation returns without writing when there is no model, the dialog
     * is canceled, or the chosen file cannot be opened.
     */
    void writeToCSV() const;

    /**
     * @brief Starts differential-expression calculation for the current selections.
     *
     * Captures the active inputs, runs data access and statistics in a workflow
     * worker stage, and builds the table in a GUI-thread stage. A concurrent
     * calculation is ignored; errors are reported after workflow completion.
     */
    void computeDE();
    
    /**
     * @brief Creates or updates the expression dataset for a clicked result row.
     * @param index Index from the table view; proxy indices are mapped to the source model.
     * Invalid indices and stale table results are ignored. Dataset-access failures
     * are reported to the user in a warning dialog.
     */
    void tableView_clicked(const QModelIndex& index);

    /**
     * @brief Updates the selected-dimension actions after table selection changes.
     * @param selected Current selection; the first valid selected index is used.
     * @param deselected Previously selected indexes; not used by this handler.
     */
    void tableView_selectionChanged(const QItemSelection& selected, const QItemSelection& deselected);


protected:

    /** Reports progress in the range [0, 1] and the current computation phase. */
    using ProgressCallback = std::function<void(float, const QString&)>;

    /**
     * @brief Computes or restores per-dimension normalization values.
     *
     * Reads cached dimension statistics when available; otherwise scans the
     * dataset and updates the supplied statistics map. It does not write the
     * map back to the dataset. The caller persists it after worker execution.
     * Dataset-access exceptions propagate to the workflow executor.
     *
     * @param points Point dataset providing the dimensions and values to scan
     * when cached statistics are missing.
     * @param minValues Receives the minimum value for each dimension.
     * @param rescaleValues Receives the reciprocal range for each dimension,
     * or 1 for a constant dimension.
     * @param dimensionStatistics Cached statistics to read and update.
     * @param reportProgress Reports normalization progress within the workflow.
     */
    static void updateNormalization(const mv::Dataset<Points>& points, std::vector<float>& minValues, std::vector<float>& rescaleValues, QVariantMap& dimensionStatistics, const ProgressCallback& reportProgress);

    /**
     * @brief Computes differential-expression values in a workflow worker job.
     *
     * The function reads point data and fills @p result without accessing plugin
     * widgets or actions. It must run on a worker thread. Exceptions propagate
     * to the workflow executor; failures caught during computation are also
     * copied into the result for UI reporting.
     *
     * The dataset must be valid and have at least one dimension, and both row
     * selections must be non-empty. Violations throw std::invalid_argument.
     *
     * @param points Dataset handle retained for the duration of the worker job.
     * @param selectionRowsA Sorted point-row indices for selection A.
     * @param selectionRowsB Sorted point-row indices for selection B.
     * @param normalize Whether to normalize values and resulting means.
     * @param additionalCalculations Whether to calculate standard deviations
     * and percentages of expressed values.
     * @param threshold Strict expression cutoff for percentage calculations;
     * values equal to the threshold are not counted as expressed.
     * @param temporaryDirectory Directory for intermediate sample data.
     * @param reportProgress Callback for worker-stage progress updates.
     * @param result Output shared with the following GUI-thread table stage.
     */
    static void runDEComputation(mv::Dataset<Points> points, std::vector<std::uint64_t> selectionRowsA, std::vector<std::uint64_t> selectionRowsB, bool normalize, bool additionalCalculations, float threshold, QString temporaryDirectory, const ProgressCallback& reportProgress, DifferentialExpressionResult& result);

    using QLabelArray2 = std::array<QLabel, MultiTriggerAction::Size>;

    DropWidget*                             _dropWidget;                        /**< Legacy drag-and-drop widget pointer; unused by the current view. */
    mv::Dataset<Points>                     _points;                            /**< Handle to the active point dataset. */
    QLabel*                                 _currentDatasetNameLabel;           /**< Legacy dataset-name label pointer; unused by the current view. */

    MultiTriggerAction                      _setSelectionTriggerActions;        /**< Actions for storing selections A and B. */
    MultiTriggerAction                      _highlightSelectionTriggerActions;  /**< Actions for highlighting selections A and B. */
    LoadedDatasetsAction                    _loadedDatasetsAction;              /**< Selects among loaded point datasets. */
    TriggerAction                           _updateStatisticsAction;            /**< Starts differential-expression calculation. */
    StringAction                            _filterOnIdAction;                  /**< Filters table rows by dimension ID. */
    StringAction                            _selectedIdAction;                  /**< Stores the currently selected dimension ID. */
    TriggerAction                           _copyToClipboardAction;             /**< Copies selected table data. */
    TriggerAction                           _saveToCsvAction;                   /**< Exports the table to CSV. */
    TriggerAction                           _openAdditionalSettingsAction;      /**< Opens selection and calculation settings. */
    DimensionPickerAction                   _currentSelectedDimension;          /**< Tracks the selected expression dimension. */
    AdditionalSettingsDialog                _additionalSettingsDialog;          /**< Configures selections and optional calculations. */

    QLabelArray2                            _selectedCellsLabel;                /**< Displays the item count for each selection. */
    int                                     _totalTableColumns;                 /**< Number of columns in the current result table. */
    QSharedPointer<TableModel>              _tableItemModel;                    /**< Shared model containing result rows. */
    QPointer<TableSortFilterProxyModel>     _sortFilterProxyModel;              /**< Filter and sort proxy; nulls when its QObject is destroyed. */
    TableView*                              _tableView;                         /**< Table view owned by the plugin's main widget. */
    QPointer<ButtonProgressBar>             _buttonProgressBar;                 /**< Calculation button and table-status display. */
    bool                                    _computationInProgress = false;     /**< Prevents overlapping differential-expression workflows. */

    QVector<WidgetAction*>                  _serializedActions;                 /**< Actions whose state is stored with this view. */
    QByteArray                              _headerState;                       /**< Serialized table-header layout state. */

    std::vector<QTableWidgetItem*>          _geneTableItems;                    /**< Legacy gene-name item cache; unused by the current model. */
    std::vector<QTableWidgetItem*>          _diffTableItems;                    /**< Legacy result item cache; unused by the current model. */

    std::vector<float>                      _minValues;                         /**< Legacy normalization minima; computation uses worker-local buffers. */
    std::vector<float>                      _rescaleValues;                     /**< Legacy normalization scales; computation uses worker-local buffers. */

    std::vector<uint32_t>                   _selectionA;                        /**< Original point indices in selection A. */
    std::vector<uint32_t>                   _selectionB;                        /**< Original point indices in selection B. */
    std::vector<std::uint64_t>              _selectionRowsA;                    /**< Sorted dataset-row indices mapped from selection A. */
    std::vector<std::uint64_t>              _selectionRowsB;                    /**< Sorted dataset-row indices mapped from selection B. */

    // additional calculations
    ToggleAction                            _additionalCalculationsAction;      /**< Enables standard deviations and expression percentages. */
    bool                                    _useAdditionalCalculations = false; /**< Cached state of the additional-calculations toggle. */
    ToggleAction                            _normAction;                        /**< Enables min-max normalization. */
    bool                                    _norm = false;                      /**< Cached state of the normalization toggle. */
    DecimalAction                           _thresholdExpressedAction;          /**< Expression threshold for percentage calculations. */
};


// =============================================================================
// Factory
// =============================================================================
/**
 * @brief Creates Differential Expression view plugin instances.
 *
 * The factory advertises the supported dataset types and supplies the view's
 * context-menu trigger actions.
 *
 * @author Thomas Kroes
 */
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
