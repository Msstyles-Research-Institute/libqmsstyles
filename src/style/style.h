#ifndef QMSSTYLESSTYLE_H
#define QMSSTYLESSTYLE_H

#include "definitions.h"

#include <QObject>
#include <QDir>
#include <QUrl>
#include <QHash>

namespace VisualStyle
{
Q_NAMESPACE

class Class;
class Part;
class State;
class Property;

class StylePrivate;
class Style : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool invalid READ invalid NOTIFY invalidChanged)

    Q_PROPERTY(QString name READ name NOTIFY nameChanged)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(Version version READ version NOTIFY versionChanged)

    Q_PROPERTY(QList<Class *> classes READ classes NOTIFY loaded)
public:
    enum Version {
        Unknown = -1,
        WindowsXP = 0,
        WindowsLonghorn,
        WindowsVista,
        Windows7,
        Windows8,
        Windows10,
        Windows11 = 255
    };
    Q_ENUM(Version)

    using Ptr = QSharedPointer<VisualStyle::Style>;

    Style(const QString &name, const QString &path, const bool readonly, QObject *parent = nullptr);
    ~Style();

    bool invalid();

    QString name() const;
    QString path() const;
    bool isReadonly() const;
    Version version() const;

    const QList<Class *> &classes() const;
    const QList<QString> &variants() const;

    int idForClass(QString name);

    Class *getClass(int classID);
    Part *getPart(int classID, int partID);
    State *getState(int classID, int partID, int stateID);
    Property *getProperty(int classID, int partID, int stateID, IDENTIFIER nameID);

    bool load();
    bool save();

Q_SIGNALS:
    void loaded();
    void saved();

    void invalidChanged();

    void nameChanged(QString name);
    void pathChanged(QString path);
    void versionChanged(VisualStyle::Style::Version version);

private:
    std::unique_ptr<StylePrivate> d;
    friend class StylePrivate;
};

}

#endif
