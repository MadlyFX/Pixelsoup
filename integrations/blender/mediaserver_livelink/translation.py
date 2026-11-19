"""Non-destructive export adjustments, reported once per scene revision."""
import math

notices = []
geometry_errors = []


def warn(message):
    if message not in notices and len(notices) < 64:
        notices.append(message)


def geometry_error(message):
    # Geometry loss must never be hidden by dozens of harmless shader notices.
    if message not in geometry_errors and len(geometry_errors) < 64:
        geometry_errors.append(message)


def sync_feedback(changed, adjustments, errors, budget=''):
    details = list(dict.fromkeys([*errors, *adjustments]))
    if errors:
        status = 'Partial sync: ' + str(len(errors)) + ' objects could not be synced. ' + errors[0]
    else:
        status = 'Synced to Pixelsoup' if changed else 'Pixelsoup is up to date'
        if adjustments:
            status += '; some scene data was adjusted or skipped'
    if budget:
        status += '; ' + budget
    return status, '\n'.join(details[:12])


def bounded(value, low=0, high=1, default=0):
    try:
        number = float(value)
    except (ValueError, TypeError):
        return default
    return min(high, max(low, number)) if math.isfinite(number) else default
