#include "TableSortFilterProxyModel.h"

TableSortFilterProxyModel::TableSortFilterProxyModel(QObject* parent)
    :QSortFilterProxyModel(parent)
{
    m_nameRegExpFilter.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
}

bool TableSortFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{

    QAbstractItemModel* model = sourceModel();
    QModelIndex source_index = model->index(sourceRow, 0, sourceParent);
    QVariant value = model->data(source_index);
    QString qs = value.toString();

    if (!m_nameRegExpFilter.pattern().isEmpty())
    {
        if (!qs.contains(m_nameRegExpFilter))
            return false;
    }
    return true;
}

bool TableSortFilterProxyModel::filterAcceptsColumn(int source_column, const QModelIndex& source_parent) const
{
    return true;
}

bool TableSortFilterProxyModel::lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const
{
    QAbstractItemModel* model = sourceModel();
    QVariant left_value = model->data(source_left);
    QVariant right_value = model->data(source_right);
    if (left_value == "N/A")
        return true;
    if (right_value == "N/A")
        return false;
    return QSortFilterProxyModel::lessThan(source_left, source_right);
}

void TableSortFilterProxyModel::nameFilterChanged(const QString& text)
{
    m_nameRegExpFilter.setPattern(text);
    invalidate();
}